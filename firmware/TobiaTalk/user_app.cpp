// TobiaTalk app logic.
//  BOOT hold  : record a voice message (min 5 s, max 15 s), release to send
//  BOOT tap   : send a heart (in history view: replay the shown message)
//  PWR tap    : play the waiting message, else browse the last 5 messages
//  PWR 5 s    : Wi-Fi / name setup portal
// Other boards poll the relay; new messages blink the LED.
#include <stdio.h>
#include <string.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "freertos/FreeRTOS.h"
#include "user_app.h"
#include "driver/gpio.h"
#include "user_config.h"
#include "esp_log.h"
#include "src/ui_src/generated/gui_guider.h"
#include "src/i2c_bsp/i2c_bsp.h"
#include "src/power/board_power_bsp.h"
#include "audio_bsp.h"
#include "secrets.h"
#include "settings.h"
#include "setup_portal.h"
#include "ota.h"
#include "version.h"
#include <time.h>

// Implemented in TobiaTalk.ino
bool example_lvgl_lock(int timeout_ms);
void example_lvgl_unlock(void);

#define LED_PIN          3      // active low
#define MIN_RECORD_MS    5000
#define MAX_RECORD_MS    15000
#define TAP_MS           400    // BOOT released sooner than this = tap (heart)
#define SETUP_HOLD_MS    5000   // hold PWR this long to open the Wi-Fi/name setup portal
#define REPLAY_HOLD_MS   3000   // in history view: hold PWR this long to replay the shown message
#define SAVER_MS         15000  // idle this long -> screensaver
#define OFFLINE_MS       20000  // Wi-Fi down this long -> "No WiFi" screen
#define BLINK_MS         5000   // how long the LED blinks for a new message
#ifndef BATT_DEBUG
#define BATT_DEBUG 0            // 1 = log battery voltage every 2 s (and keep it in flash; send 'd' to dump, 'c' to clear)
#endif
#define POWEROFF_HOLD_MS 10000  // hold PWR+BOOT this long -> power off (battery only); deliberate on purpose
#define POLL_EVERY_MS   20000
#define MONO_CAP_SAMPLES (16000 * 20)   // matches the server's 20 s limit

epaper_driver_display *driver = NULL;
board_power_bsp_t board_div(EPD_PWR_PIN, Audio_PWR_PIN, VBAT_PWR_PIN);

static lv_ui src_ui;
static lv_obj_t *heart_canvas = NULL;
static lv_color_t heart_buf[LV_CANVAS_BUF_SIZE_TRUE_COLOR(80, 70)];
static lv_obj_t *envelope_canvas = NULL;
static lv_color_t envelope_buf[LV_CANVAS_BUF_SIZE_TRUE_COLOR(80, 56)];
static lv_obj_t *mini_env_canvas = NULL;   // tiny envelope, bottom-right of the screensaver: unread message
static lv_color_t mini_env_buf[LV_CANVAS_BUF_SIZE_TRUE_COLOR(28, 20)];
static lv_obj_t *off_label = NULL;      // "OFF" tag, bottom-left: drawn just before a battery board powers down
static lv_obj_t *foot_label = NULL;     // small line at the bottom of the screen
static lv_obj_t *corner_label = NULL;   // small tag in the top-right corner (firmware version)
enum Icon { ICON_NONE, ICON_HEART, ICON_ENVELOPE };
static uint32_t last_activity = 0;      // when the send screen was last shown (for the screensaver)
static int16_t *mono = NULL;            // 16 kHz mono PCM, lives in PSRAM
static Settings g_set;                  // Wi-Fi + this person's name (from flash)
static int g_last_id = 0;
static bool g_first_poll = true;
static int g_fw_offer_version = 0;      // newest firmware the server will give this board (0 = none)
static size_t g_fw_offer_size = 0;
static int g_ota_failed_version = 0;    // a version that failed to install is not retried for an hour...
static uint32_t g_ota_retry_after = 0;  // ...but a different (newer) version is tried right away
static int g_tls_fail = 0;
static bool g_tls_insecure = false;
static lv_obj_t *batt_label = NULL;     // top-left: battery symbol + percent, only when a battery is connected
static int g_batt_pct = -2;             // last shown: -2 = not drawn yet, -1 = no battery
static bool g_batt_seen = false;        // a battery-only voltage has been measured since boot
static void battery_note_activity();    // defined with the other battery code below

static int g_pending_id = 0;            // newest unseen message from someone else
static bool g_pending_heart = false;
static char g_pending_from[24] = "";
static uint32_t g_blink_until = 0;      // LED blinks until this time after a new message arrives

struct Msg { int id; char from[24]; bool heart; int seconds; uint32_t age; };
enum View { V_IDLE, V_HISTORY, V_SAVER };
static View view = V_IDLE;
static Msg hist[5];                     // newest first
static int hist_n = 0, hist_i = 0;
static uint32_t hist_fetched = 0, view_deadline = 0;

static void set_status(const char *top, const char *bottom, Icon icon = ICON_NONE, const char *foot = NULL,
                       const char *corner = NULL, bool mini_env = false)
{
  if (example_lvgl_lock(-1)) {
    lv_obj_add_flag(off_label, LV_OBJ_FLAG_HIDDEN);   // any normal screen clears the OFF tag
    if (mini_env) lv_obj_clear_flag(mini_env_canvas, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(mini_env_canvas, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(src_ui.screen_label_1, top);
    lv_label_set_text(src_ui.screen_label_2, bottom);
    if (icon == ICON_HEART) lv_obj_clear_flag(heart_canvas, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(heart_canvas, LV_OBJ_FLAG_HIDDEN);
    if (icon == ICON_ENVELOPE) lv_obj_clear_flag(envelope_canvas, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(envelope_canvas, LV_OBJ_FLAG_HIDDEN);
    if (foot) {
      lv_label_set_text(foot_label, foot);
      lv_obj_clear_flag(foot_label, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(foot_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (corner) {
      lv_label_set_text(corner_label, corner);
      lv_obj_clear_flag(corner_label, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(corner_label, LV_OBJ_FLAG_HIDDEN);
    }
    example_lvgl_unlock();
  }
}

static void set_led(bool on) { gpio_set_level((gpio_num_t)LED_PIN, on ? 0 : 1); }
static bool button_down(int pin) { return gpio_get_level((gpio_num_t)pin) == 0; }
static bool online() { return WiFi.status() == WL_CONNECTED; }

static bool g_offline = false;          // Wi-Fi has been down long enough to tell the user

// The "send a message" screen: where every action returns to.
static void show_idle()
{
  view = V_IDLE;
  last_activity = millis();
  char buf[40];
  if (g_offline) {                       // stays up until Wi-Fi is back, whatever else was on screen
    set_status("No WiFi", "Hold PWR 5s", ICON_NONE, "to set up WiFi");
  } else if (g_pending_id && g_pending_heart) {
    set_status(g_pending_from, "sent a heart", ICON_HEART);
  } else if (g_pending_id) {
    snprintf(buf, sizeof(buf), "from %s", g_pending_from);
    set_status("New message", buf, ICON_ENVELOPE);
  } else {
    set_status(g_set.name.c_str(), "Hold to talk", ICON_NONE, "Tap = heart");
  }
}

// Screensaver: e-paper keeps this image with no power, so it is also what an
// unplugged board shows. Any tap goes back to the send screen.
static void show_saver()
{
  view = V_SAVER;
  char version[12];
  snprintf(version, sizeof(version), "v%d", FW_VERSION);
  set_status("Tobia Talk", "Device", ICON_HEART, g_set.name.c_str(), version, g_pending_id != 0);
}

static void format_age(char *out, size_t n, uint32_t s)
{
  if (s < 60) snprintf(out, n, "just now");
  else if (s < 3600) snprintf(out, n, "%um ago", (unsigned)(s / 60));
  else if (s < 86400) snprintf(out, n, "%uh ago", (unsigned)(s / 3600));
  else snprintf(out, n, "%ud ago", (unsigned)(s / 86400));
}

static void wifi_start()
{
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(g_set.ssid.c_str(), g_set.pass.c_str());
  configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");   // clock is needed to check certificates
}

static bool time_valid() { return time(nullptr) > 1700000000; }   // SNTP has set the clock

// Certificates are only checked once the clock is right. If verified connections keep
// failing (e.g. the server's CA moved out of the built-in list) we fall back to
// unverified TLS until the next reboot, so boards stay reachable for updates.
static void tls_setup(WiFiClientSecure &tls)
{
  if (time_valid() && !g_tls_insecure) tls.useBuiltinCACertBundle();
  else tls.setInsecure();
}

// HTTP status < 0 means the connection itself failed (DNS, TLS, timeout).
static void note_transport(int code)
{
  if (code >= 0) { g_tls_fail = 0; return; }
  if (++g_tls_fail >= 3 && time_valid()) g_tls_insecure = true;
}

static String name_param()
{
  String n = g_set.name;
  n.replace(" ", "%20");
  return n;
}

static bool begin_request(HTTPClient &http, WiFiClientSecure &tls, const String &path)
{
  tls_setup(tls);
  http.setTimeout(15000);
  if (!http.begin(tls, String(SERVER_URL) + path)) return false;
  http.addHeader("x-family-key", FAMILY_KEY);
  return true;
}

static void poll_messages()
{
  WiFiClientSecure tls;
  HTTPClient http;
  String path = "/messages?since=" + String(g_last_id) + "&v=" + String(FW_VERSION) + "&me=" + name_param();
  if (!begin_request(http, tls, path)) return;
  int code = http.GET();
  note_transport(code);
  if (code != 200) { http.end(); return; }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getString());
  http.end();
  if (err) return;
  ota_mark_valid();                               // a new firmware proved it can talk to the server
  g_fw_offer_version = doc["fw"]["version"] | 0;  // newer signed firmware on offer (0 = none)
  g_fw_offer_size = doc["fw"]["size"] | 0;
  bool changed = false;
  for (JsonObject m : doc["messages"].as<JsonArray>()) {
    int id = m["id"];
    if (id > g_last_id) g_last_id = id;
    const char *from = m["from"] | "Someone";
    if (!g_first_poll && g_set.name != from) {   // skip history and our own messages
      g_pending_id = id;
      g_pending_heart = strcmp(m["type"] | "voice", "heart") == 0;
      strlcpy(g_pending_from, from, sizeof(g_pending_from));
      changed = true;
    }
  }
  g_first_poll = false;
  if (changed) g_blink_until = millis() + BLINK_MS;   // blink the LED for a few seconds, then stop
  if (changed && view != V_HISTORY) show_idle();   // also wakes the screensaver
}

static int fetch_history()
{
  WiFiClientSecure tls;
  HTTPClient http;
  // The server leaves out this board's own messages, so history only shows what others sent.
  if (!begin_request(http, tls, "/messages?last=5&exclude=" + name_param())) return 0;
  if (http.GET() != 200) { http.end(); return 0; }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getString());
  http.end();
  if (err) return 0;
  JsonArray arr = doc["messages"].as<JsonArray>();
  int n = 0;
  for (int i = (int)arr.size() - 1; i >= 0 && n < 5; i--) {   // server is oldest-first
    JsonObject m = arr[i];
    hist[n].id = m["id"];
    strlcpy(hist[n].from, m["from"] | "Someone", sizeof(hist[n].from));
    hist[n].heart = strcmp(m["type"] | "voice", "heart") == 0;
    hist[n].seconds = m["seconds"] | 0;
    hist[n].age = m["age"] | 0;
    n++;
  }
  hist_fetched = millis();
  return n;
}

static void show_history()
{
  Msg &m = hist[hist_i];
  char age[16], bottom[32];
  format_age(age, sizeof(age), m.age + (millis() - hist_fetched) / 1000);
  snprintf(bottom, sizeof(bottom), "%s %d/%d", age, hist_i + 1, hist_n);
  set_status(m.from, bottom, m.heart ? ICON_HEART : ICON_NONE, m.heart ? NULL : "Hold PWR for 3 seconds\nto play");
  view_deadline = millis() + 10000;
}

// Hold BOOT to record. Returns mono samples, -1 if BOOT was released within
// TAP_MS (a tap), or -2 if PWR was pressed together with BOOT (within TAP_MS).
// Keeps recording until MIN_RECORD_MS even if BOOT is released early, and stops
// at MAX_RECORD_MS.
static int record_message()
{
  static int16_t stereo[512];            // 256 frames of L/R
  size_t n = 0;
  bool announced = false;
  uint32_t t0 = millis();
  while (n + 256 <= MONO_CAP_SAMPLES) {
    uint32_t elapsed = millis() - t0;
    bool down = button_down(BOOT_BUTTON_PIN);
    if (elapsed < TAP_MS && button_down(PWR_BUTTON_PIN)) return -2;   // both buttons together
    if (!down && elapsed < TAP_MS) return -1;
    if (!announced && elapsed >= TAP_MS) {   // it's a hold: now show it
      announced = true;
      set_led(true);
      set_status("Recording", "Speak now");
    }
    if (elapsed >= MAX_RECORD_MS) break;
    if (!down && elapsed >= MIN_RECORD_MS) break;
    audio_playback_read(stereo, sizeof(stereo));
    for (int i = 0; i < 256; i++) mono[n++] = (stereo[2 * i] + stereo[2 * i + 1]) / 2;
  }
  set_led(false);
  battery_note_activity();               // recording, then the upload, load the battery
  return (int)n;
}

static bool send_message(size_t samples, bool heart)
{
  if (!online()) return false;
  WiFiClientSecure tls;
  HTTPClient http;
  String path = "/send?from=" + g_set.name;
  path.replace(" ", "%20");
  if (heart) path += "&type=heart";
  if (!begin_request(http, tls, path)) return false;
  http.addHeader("content-type", "application/octet-stream");
  int code = heart ? http.POST("") : http.POST((uint8_t *)mono, samples * 2);
  http.end();
  battery_note_activity();
  return code == 200;
}

static size_t fetch_audio(int id)
{
  WiFiClientSecure tls;
  HTTPClient http;
  if (!begin_request(http, tls, "/audio?id=" + String(id))) return 0;
  if (http.GET() != 200) { http.end(); return 0; }
  int len = http.getSize();              // needs Content-Length (worker sets it)
  if (len <= 0 || len > MONO_CAP_SAMPLES * 2) { http.end(); return 0; }
  WiFiClient *s = http.getStreamPtr();
  uint8_t *dst = (uint8_t *)mono;
  int got = 0;
  uint32_t t0 = millis();
  while (got < len && millis() - t0 < 30000 && (http.connected() || s->available())) {
    int avail = s->available();
    if (avail) got += s->readBytes(dst + got, min(avail, len - got));
    else vTaskDelay(pdMS_TO_TICKS(5));
  }
  http.end();
  return got == len ? len / 2 : 0;
}

static void play_samples(size_t samples)
{
  static int16_t stereo[1024];           // 512 frames
  for (size_t i = 0; i < samples; i += 512) {
    size_t n = min((size_t)512, samples - i);
    for (size_t j = 0; j < n; j++) stereo[2 * j] = stereo[2 * j + 1] = mono[i + j];
    audio_playback_write(stereo, n * 4);
  }
}

// Returns false if the audio could not be fetched (expired or offline).
static bool play_message(int id, const char *from)
{
  set_status("Loading", from);
  size_t n = online() ? fetch_audio(id) : 0;
  if (!n) {
    set_status("Message", "expired");
    vTaskDelay(pdMS_TO_TICKS(2000));
    return false;
  }
  set_status("Playing", from);
  play_samples(n);
  battery_note_activity();               // the speaker load sagged the battery; don't read that as an unplug
  return true;
}

// The signature, not TLS, is what makes a download trustworthy, so a failed verified
// connection is retried once unverified (otherwise a changed CA could block the fix).
static bool run_ota(String &err)
{
  for (int attempt = 0; attempt < 2; attempt++) {
    WiFiClientSecure tls;
    if (attempt == 0) tls_setup(tls);
    else tls.setInsecure();
    if (ota_install(tls, SERVER_URL, FAMILY_KEY, g_fw_offer_version, g_fw_offer_size, err)) return true;
    if (!(err == "connect" || err.startsWith("http -"))) break;   // only retry connection failures
  }
  return false;
}

static void enter_setup()
{
  set_status("Join WiFi", "TobiaTalk");
  setup_portal_run("TobiaTalk");         // restarts the board when done
}

static void on_boot_tap()
{
  if (view == V_SAVER) {                 // a tap on the screensaver only wakes it (no accidental heart)
    show_idle();
    return;
  }
  set_status("Sending", "heart", ICON_HEART);
  bool ok = send_message(0, true);
  set_status(ok ? "Sent" : "Failed", ok ? "heart" : online() ? "Try again" : "No WiFi", ok ? ICON_HEART : ICON_NONE);
  vTaskDelay(pdMS_TO_TICKS(2500));
  show_idle();
}

static void on_pwr_short()
{
  if (view == V_SAVER) {                 // wake the screensaver back to the send screen
    show_idle();
    return;
  }
  if (view == V_HISTORY) {               // in history PWR only pages down (next older message), wrapping at the end
    hist_i = (hist_i + 1) % hist_n;
    show_history();
    return;
  }
  if (g_pending_id) {                    // new message waiting: play it / acknowledge the heart
    int id = g_pending_id;
    bool heart = g_pending_heart;
    char from[24];
    strlcpy(from, g_pending_from, sizeof(from));
    g_pending_id = 0;
    if (!heart) play_message(id, from);
    show_idle();
  } else if (view == V_HISTORY) {
    hist_i = (hist_i + 1) % hist_n;
    show_history();
  } else {
    set_status("Loading", "history");
    hist_n = online() ? fetch_history() : 0;
    if (!hist_n) {
      set_status("No messages", online() ? "yet" : "No WiFi");
      vTaskDelay(pdMS_TO_TICKS(2000));
      show_idle();
      return;
    }
    view = V_HISTORY;
    hist_i = 0;
    show_history();
  }
}

// ---- battery ----
// The battery is read through a 1:2 divider on GPIO4 (ADC1 channel 3), as in Waveshare's ADC example.
#define BATT_ADC_PIN 4

static int battery_mv()
{
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(BATT_ADC_PIN);
  return (int)(sum / 16) * 2;
}

// Typical single-cell LiPo discharge curve (open-circuit volts -> percent).
static int battery_percent(int mv)
{
  static const int table[][2] = {{4200, 100}, {4100, 90}, {4000, 80}, {3900, 65}, {3800, 45},
                                 {3750, 30},  {3700, 20}, {3650, 12}, {3600, 6},  {3300, 0}};
  if (mv >= table[0][0]) return 100;
  for (size_t i = 1; i < sizeof(table) / sizeof(table[0]); i++) {
    if (mv >= table[i][0]) {
      int hi_mv = table[i - 1][0], lo_mv = table[i][0], hi_p = table[i - 1][1], lo_p = table[i][1];
      return lo_p + (hi_p - lo_p) * (mv - lo_mv) / (hi_mv - lo_mv);
    }
  }
  return 0;
}

// While USB is plugged in the charger pushes current into the battery and the pin reads ~65 mV
// ABOVE the battery's own voltage; on battery the board's own draw pulls it a bit down. Neither is the
// true level, but the switch between them is a sharp step (60-72 mV measured, against <= 18 mV of
// everyday noise). So we track "plugged in / on battery" from those steps and, while plugged in,
// subtract the offset learned from the last step and show a charging bolt.
#define BATT_STEP_MV 45                 // a jump this big, holding for the next reading, is a plug/unplug
static bool g_usb = true;               // assume plugged in at boot; the first step corrects it
static int g_offset_mv = 65;
static int bat_win[6];                  // last few 2-second readings
static int bat_win_n = 0;
static bool bat_cand = false;           // a possible step waiting for confirmation
static int bat_cand_mv = 0;
static uint32_t g_bat_quiet_until = 0;
static bool g_batt_usb_shown = true;

// Audio playback, recording and uploads load the battery and move the voltage; ignore it and re-baseline after.
static void battery_note_activity()
{
  g_bat_quiet_until = millis() + 4000;
  bat_win_n = 0;
  bat_cand = false;
}

// Draws the indicator. The e-paper redraws the whole screen on any change, so the label only changes
// when the percent moves by 4+ points, or the plugged/unplugged state flips.
static void battery_show(int mv_avg)
{
  bool present = g_batt_seen && mv_avg >= 3000 && mv_avg <= 4400;
  int raw = (g_usb && mv_avg >= 4180) ? 100 : battery_percent(g_usb ? mv_avg - g_offset_mv : mv_avg);
  int pct;
  if (!present) pct = -1;
  else if (g_batt_pct >= 0 && abs(raw - g_batt_pct) < 4 && g_usb == g_batt_usb_shown) pct = g_batt_pct;
  else pct = (raw + 2) / 5 * 5;
  if (pct == g_batt_pct && g_usb == g_batt_usb_shown) return;
  g_batt_pct = pct;
  g_batt_usb_shown = g_usb;
  if (!example_lvgl_lock(-1)) return;
  if (pct < 0) {
    lv_obj_add_flag(batt_label, LV_OBJ_FLAG_HIDDEN);
  } else {
    const char *sym = g_usb ? LV_SYMBOL_CHARGE
                    : pct > 87 ? LV_SYMBOL_BATTERY_FULL : pct > 62 ? LV_SYMBOL_BATTERY_3
                    : pct > 37 ? LV_SYMBOL_BATTERY_2 : pct > 12 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
    char text[24];
    snprintf(text, sizeof(text), "%s %d%%", sym, pct);
    lv_label_set_text(batt_label, text);
    lv_obj_clear_flag(batt_label, LV_OBJ_FLAG_HIDDEN);
  }
  example_lvgl_unlock();
}

// Called every ~2 s.
static void battery_sample()
{
  int mv = battery_mv();
  if (millis() < g_bat_quiet_until) return;
  if (bat_win_n >= 3) {
    int sum = 0;
    for (int i = 0; i < bat_win_n; i++) sum += bat_win[i];
    int avg = sum / bat_win_n;
    if (!bat_cand && abs(mv - avg) >= BATT_STEP_MV) { bat_cand = true; bat_cand_mv = mv; return; }
    if (bat_cand) {
      bat_cand = false;
      if (abs(mv - bat_cand_mv) < 20) {                 // it held: a real plug / unplug, not a load spike
        int step = bat_cand_mv - avg;
        g_usb = step > 0;
        g_offset_mv = (g_offset_mv + constrain(abs(step), 30, 120)) / 2;
        g_batt_seen = true;                             // a battery-less board loses power on unplug, so a step proves one
        bat_win[0] = bat_cand_mv;
        bat_win[1] = mv;
        bat_win_n = 2;
        Serial.printf("[battery] step %+d mV -> %s, offset %d mV\n", step, g_usb ? "plugged in" : "on battery", g_offset_mv);
        battery_show((bat_cand_mv + mv) / 2);
        return;
      }
      // it did not hold: a transient, so use this reading normally
    }
  }
  if (bat_win_n < 6) {
    bat_win[bat_win_n++] = mv;
  } else {
    for (int i = 0; i < 5; i++) bat_win[i] = bat_win[i + 1];
    bat_win[5] = mv;
  }
  if (mv >= 3000 && mv <= 4050) g_batt_seen = true;    // below the ~4.17 V a battery-less board reads on USB
  if (bat_win_n >= 2) {
    int sum = 0;
    for (int i = 0; i < bat_win_n; i++) sum += bat_win[i];
    battery_show(sum / bat_win_n);
  }
}

// What one BOOT press means in history view.
enum { BG_TAP, BG_HOLD, BG_COMBO };
static int boot_gesture()
{
  uint32_t t0 = millis();
  for (;;) {
    uint32_t elapsed = millis() - t0;
    if (elapsed < TAP_MS && button_down(PWR_BUTTON_PIN)) return BG_COMBO;
    if (!button_down(BOOT_BUTTON_PIN) && elapsed < TAP_MS) return BG_TAP;
    if (elapsed >= TAP_MS) return BG_HOLD;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// PWR + BOOT together: history -> send screen, send screen -> screensaver, screensaver stays.
// Holding both for 10 s powers off a battery-powered board with the screensaver (tagged OFF) left on the e-paper.
static void handle_combo()
{
  if (view == V_HISTORY) show_idle();
  else if (view == V_IDLE) show_saver();

  uint32_t both_t0 = millis();
  bool powered_off = false;
  while (button_down(BOOT_BUTTON_PIN) || button_down(PWR_BUTTON_PIN)) {
    if (!powered_off && button_down(BOOT_BUTTON_PIN) && button_down(PWR_BUTTON_PIN) &&
        millis() - both_t0 >= POWEROFF_HOLD_MS) {
      powered_off = true;
      if (view != V_SAVER) show_saver();   // always power off showing the screensaver
      if (example_lvgl_lock(-1)) {         // mark it OFF so a dark board does not look merely idle
        lv_obj_clear_flag(off_label, LV_OBJ_FLAG_HIDDEN);
        example_lvgl_unlock();
      }
      vTaskDelay(pdMS_TO_TICKS(3500));     // let the e-paper finish drawing first
      board_div.VBAT_POWER_OFF();          // on USB power the board keeps running; see below
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  if (powered_off) {                       // still alive = USB powered: re-latch so unplugging USB keeps it running
    board_div.VBAT_POWER_ON();
    show_saver();                          // and redraw without the OFF tag
  }
  vTaskDelay(pdMS_TO_TICKS(150));          // let the contacts settle
}

static void app_task(void *arg)
{
  if (!settings_load(g_set)) {           // never configured: go straight to setup
    enter_setup();
  }
  set_status("Connecting", "WiFi...");
  wifi_start();
  for (int i = 0; i < 40 && !online(); i++) vTaskDelay(pdMS_TO_TICKS(500));
  uint32_t last_poll = 0, pwr_t0 = 0, offline_since = millis(), last_batt = 0, last_dbg = 0;
  bool pwr_was_down = false, pwr_consumed = false;
  g_offline = !online();                 // the 20 s connect attempt above already failed: say so right away
  show_idle();

  for (;;) {
    if (button_down(BOOT_BUTTON_PIN)) {
      int n = 0;
      if (view == V_HISTORY) {           // in history BOOT pages up; it does not record (PWR hold replays)
        int g = boot_gesture();
        if (g == BG_COMBO) {
          handle_combo();
          pwr_was_down = false;
        } else if (g == BG_TAP) {        // previous message (wraps from the first to the last)
          hist_i = (hist_i + hist_n - 1) % hist_n;
          show_history();
        } else {                         // BOOT held in history: nothing to do, ignore until released
          while (button_down(BOOT_BUTTON_PIN)) vTaskDelay(pdMS_TO_TICKS(20));
        }
        last_poll = millis();
        continue;
      }
      n = record_message();
      if (n == -2) {                     // PWR + BOOT together
        handle_combo();
        pwr_was_down = false;            // the PWR release must not count as a tap
      } else if (n < 0) {
        on_boot_tap();
      } else {
        set_status("Sending", "...");
        bool ok = send_message(n, false);
        set_status(ok ? "Sent" : "Failed", ok ? g_set.name.c_str() : online() ? "Try again" : "No WiFi");
        vTaskDelay(pdMS_TO_TICKS(2500));
        show_idle();
      }
      last_poll = millis();
    }

    bool pwr_down = button_down(PWR_BUTTON_PIN);
    if (pwr_down && !pwr_was_down) { pwr_t0 = millis(); pwr_consumed = false; }
    if (pwr_down && !pwr_consumed && view == V_HISTORY && millis() - pwr_t0 >= REPLAY_HOLD_MS) {
      pwr_consumed = true;               // held in history: replay the shown message (and no page-down / setup on release)
      Msg &m = hist[hist_i];
      if (!m.heart) play_message(m.id, m.from);
      show_history();
      last_poll = millis();
    }
    if (pwr_down && !pwr_consumed && millis() - pwr_t0 >= SETUP_HOLD_MS) enter_setup();
    if (!pwr_down && pwr_was_down && !pwr_consumed && millis() - pwr_t0 > 30) on_pwr_short();
    pwr_was_down = pwr_down;

#if BATT_DEBUG
    // TEMPORARY experiment: log the battery voltage every 2 s so plug/unplug jumps can be measured.
    // Readings are also kept in memory (30 min) so an unplugged stretch can be read back: send 'd' over serial.
    static uint32_t ring_t[900];
    static uint16_t ring_mv[900];
    static int ring_n = 0;
    static bool ring_loaded = false;
    static uint32_t last_save = 0;
    if (!ring_loaded) {   // opening the serial port resets this board, so keep the readings in flash across restarts
      ring_loaded = true;
      Preferences rp;
      rp.begin("battdbg", true);
      ring_n = rp.getBytes("t", ring_t, sizeof(ring_t)) / 4;
      int nm = rp.getBytes("mv", ring_mv, sizeof(ring_mv)) / 2;
      if (nm < ring_n) ring_n = nm;
      rp.end();
      if (ring_n < 900) { ring_t[ring_n] = 0; ring_mv[ring_n] = 0; ring_n++; }   // t=0 mV=0 marks a boot
    }
    if (millis() - last_dbg > 2000) {
      int mv = battery_mv();
      if (ring_n < 900) { ring_t[ring_n] = millis(); ring_mv[ring_n] = mv; ring_n++; }
      Serial.printf("[batt-dbg] t=%lu mV=%d\n", (unsigned long)millis(), mv);
      last_dbg = millis();
    }
    if (millis() - last_save > 20000 && ring_n > 0) {
      Preferences rp;
      rp.begin("battdbg", false);
      rp.putBytes("t", ring_t, ring_n * 4);
      rp.putBytes("mv", ring_mv, ring_n * 2);
      rp.end();
      last_save = millis();
    }
    if (Serial.available()) {
      int ch = Serial.read();
      if (ch == 'd') {         // dump everything recorded so far
        for (int i = 0; i < ring_n; i++) Serial.printf("[ring] t=%lu mV=%u\n", (unsigned long)ring_t[i], ring_mv[i]);
        Serial.println("[ring] end");
      } else if (ch == 'c') {  // clear the recording to start a fresh run
        ring_n = 0;
        Preferences rp;
        rp.begin("battdbg", false);
        rp.clear();
        rp.end();
        Serial.println("[ring] cleared");
      }
    }
#endif

    if (view == V_HISTORY && millis() > view_deadline) show_idle();
    // A waiting message no longer blocks the screensaver: it shows a little envelope in the corner.
    if (view == V_IDLE && !g_offline && millis() - last_activity > SAVER_MS) show_saver();

    // Automatic update: only while idle on the screensaver, nothing waiting, and online.
    if (g_fw_offer_version > FW_VERSION && view == V_SAVER && !g_pending_id && online() &&
        (g_fw_offer_version != g_ota_failed_version || millis() > g_ota_retry_after)) {
      set_status("Updating", "Do not unplug");
      String err;
      if (!run_ota(err)) {                // success restarts the board and never gets here
        set_status("Update failed", err.c_str());
        vTaskDelay(pdMS_TO_TICKS(4000));
        g_ota_failed_version = g_fw_offer_version;
        g_ota_retry_after = millis() + 60UL * 60 * 1000;
        show_saver();
      }
    }

    // A just-installed firmware that cannot reach the server within 3 minutes goes back to the old one.
    if (millis() > 180000 && ota_pending_verify()) ota_rollback();

    // New-message LED: blinks for BLINK_MS after the message arrives, then goes dark (the envelope stays on screen).
    static bool blinking = false;
    if ((int32_t)(g_blink_until - millis()) > 0) {
      blinking = true;
      set_led((millis() / 300) % 2 == 0);
    } else if (blinking) {
      blinking = false;
      set_led(false);
    }

    if (online()) {
      offline_since = millis();
      if (g_offline) { g_offline = false; show_idle(); }
    } else if (!g_offline && millis() - offline_since > OFFLINE_MS) {
      g_offline = true;
      if (view != V_HISTORY) show_idle();
    }

    if (millis() - last_batt >= 2000) {
      battery_sample();
      last_batt = millis();
    }

    if (millis() - last_poll > POLL_EVERY_MS) {
      if (online()) poll_messages();
      last_poll = millis();
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void user_app_init(void)
{
  mono = (int16_t *)heap_caps_malloc(MONO_CAP_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  board_div.VBAT_POWER_ON();             // latch: on battery the board would shut off when PWR is released
  board_div.POWEER_EPD_ON();
  board_div.POWEER_Audio_ON();
  i2c_master_Init();

  custom_lcd_spi_t cfg = {};
  cfg.cs = EPD_CS_PIN;
  cfg.dc = EPD_DC_PIN;
  cfg.rst = EPD_RST_PIN;
  cfg.busy = EPD_BUSY_PIN;
  cfg.mosi = EPD_MOSI_PIN;
  cfg.scl = EPD_SCK_PIN;
  cfg.spi_host = EPD_SPI_NUM;
  cfg.buffer_len = 5000;
  driver = new epaper_driver_display(EPD_WIDTH, EPD_HEIGHT, cfg);
  driver->EPD_Init();
  driver->EPD_Clear();
  driver->EPD_DisplayPartBaseImage();
  driver->EPD_Init_Partial();

  gpio_config_t out = {};
  out.mode = GPIO_MODE_OUTPUT;
  out.pin_bit_mask = 1ULL << LED_PIN;
  gpio_config(&out);
  set_led(false);

  gpio_config_t in = {};                 // buttons are active low
  in.mode = GPIO_MODE_INPUT;
  in.pull_up_en = GPIO_PULLUP_ENABLE;
  in.pin_bit_mask = (1ULL << BOOT_BUTTON_PIN) | (1ULL << PWR_BUTTON_PIN);
  gpio_config(&in);

  audio_bsp_init();
  audio_play_init();
}

void user_ui_init(void)
{
  setup_ui(&src_ui);
  lv_label_set_text(src_ui.screen_label_1, "Starting");
  lv_label_set_text(src_ui.screen_label_2, "");

  // Heart drawn pixel by pixel: (x^2+y^2-1)^3 - x^2*y^3 <= 0 is a heart.
  heart_canvas = lv_canvas_create(src_ui.screen);
  lv_canvas_set_buffer(heart_canvas, heart_buf, 80, 70, LV_IMG_CF_TRUE_COLOR);
  lv_canvas_fill_bg(heart_canvas, lv_color_white(), LV_OPA_COVER);
  for (int py = 0; py < 70; py++) {
    for (int px = 0; px < 80; px++) {
      float x = (px - 39.5f) / 28.0f, y = (37.0f - py) / 28.0f;
      float a = x * x + y * y - 1.0f;
      if (a * a * a - x * x * y * y * y <= 0) lv_canvas_set_px(heart_canvas, px, py, lv_color_black());
    }
  }
  lv_obj_set_pos(heart_canvas, 60, 2);
  lv_obj_add_flag(heart_canvas, LV_OBJ_FLAG_HIDDEN);

  // Envelope (new-message icon): 3 px border, flap lines meeting in the middle.
  envelope_canvas = lv_canvas_create(src_ui.screen);
  lv_canvas_set_buffer(envelope_canvas, envelope_buf, 80, 56, LV_IMG_CF_TRUE_COLOR);
  lv_canvas_fill_bg(envelope_canvas, lv_color_white(), LV_OPA_COVER);
  for (int py = 0; py < 56; py++) {
    for (int px = 0; px < 80; px++) {
      bool inside = px >= 4 && px <= 75 && py >= 4 && py <= 51;
      bool border = inside && (px < 7 || px > 72 || py < 7 || py > 48);
      float flap = px <= 40 ? 4.0f + 26.0f * (px - 4) / 36.0f : 4.0f + 26.0f * (75 - px) / 35.0f;
      bool flap_px = inside && fabsf(py - flap) <= 1.5f;
      if (border || flap_px) lv_canvas_set_px(envelope_canvas, px, py, lv_color_black());
    }
  }
  lv_obj_set_pos(envelope_canvas, 60, 12);
  lv_obj_add_flag(envelope_canvas, LV_OBJ_FLAG_HIDDEN);

  // Same envelope, 28x20, for the bottom-right of the screensaver (2 px border, flap to the middle).
  mini_env_canvas = lv_canvas_create(src_ui.screen);
  lv_canvas_set_buffer(mini_env_canvas, mini_env_buf, 28, 20, LV_IMG_CF_TRUE_COLOR);
  lv_canvas_fill_bg(mini_env_canvas, lv_color_white(), LV_OPA_COVER);
  for (int py = 0; py < 20; py++) {
    for (int px = 0; px < 28; px++) {
      bool border = px < 2 || px > 25 || py < 2 || py > 17;
      float flap = px <= 13 ? 1.0f + 9.0f * px / 13.0f : 1.0f + 9.0f * (27 - px) / 14.0f;
      bool flap_px = px >= 1 && px <= 26 && fabsf(py - flap) <= 1.0f;
      if (border || flap_px) lv_canvas_set_px(mini_env_canvas, px, py, lv_color_black());
    }
  }
  lv_obj_set_pos(mini_env_canvas, 166, 172);
  lv_obj_add_flag(mini_env_canvas, LV_OBJ_FLAG_HIDDEN);

  foot_label = lv_label_create(src_ui.screen);
  lv_label_set_text(foot_label, "");
  lv_obj_set_size(foot_label, 170, LV_SIZE_CONTENT);       // grows with its text (the history hint takes two lines)
  lv_obj_align(foot_label, LV_ALIGN_BOTTOM_MID, 0, -8);    // anchored to the bottom so extra lines grow upward
  lv_obj_set_style_text_font(foot_label, &lv_font_montserrat_14, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_color(foot_label, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_align(foot_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_add_flag(foot_label, LV_OBJ_FLAG_HIDDEN);

  off_label = lv_label_create(src_ui.screen);
  lv_label_set_text(off_label, "OFF");
  lv_obj_set_pos(off_label, 8, 172);
  lv_obj_set_size(off_label, 40, 20);
  lv_obj_set_style_text_font(off_label, &lv_font_montserrat_14, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_color(off_label, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_add_flag(off_label, LV_OBJ_FLAG_HIDDEN);

  batt_label = lv_label_create(src_ui.screen);
  lv_label_set_text(batt_label, "");
  lv_obj_set_pos(batt_label, 6, 4);
  lv_obj_set_size(batt_label, 62, 18);
  lv_obj_set_style_text_font(batt_label, &lv_font_montserrat_14, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_color(batt_label, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_add_flag(batt_label, LV_OBJ_FLAG_HIDDEN);

  corner_label = lv_label_create(src_ui.screen);
  lv_label_set_text(corner_label, "");
  lv_obj_set_pos(corner_label, 144, 4);
  lv_obj_set_size(corner_label, 52, 18);
  lv_obj_set_style_text_font(corner_label, &lv_font_montserrat_14, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_color(corner_label, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_align(corner_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_add_flag(corner_label, LV_OBJ_FLAG_HIDDEN);

  xTaskCreatePinnedToCore(app_task, "app_task", 16 * 1024, NULL, 3, NULL, 0);
}
