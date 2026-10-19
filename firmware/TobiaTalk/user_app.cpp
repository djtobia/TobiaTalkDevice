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
#define SAVER_MS         15000  // idle this long -> screensaver
#define POLL_EVERY_MS   20000
#define MONO_CAP_SAMPLES (16000 * 20)   // matches the server's 20 s limit

epaper_driver_display *driver = NULL;
board_power_bsp_t board_div(EPD_PWR_PIN, Audio_PWR_PIN, VBAT_PWR_PIN);

static lv_ui src_ui;
static lv_obj_t *heart_canvas = NULL;
static lv_color_t heart_buf[LV_CANVAS_BUF_SIZE_TRUE_COLOR(80, 70)];
static lv_obj_t *envelope_canvas = NULL;
static lv_color_t envelope_buf[LV_CANVAS_BUF_SIZE_TRUE_COLOR(80, 56)];
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

static int g_pending_id = 0;            // newest unseen message from someone else
static bool g_pending_heart = false;
static char g_pending_from[24] = "";

struct Msg { int id; char from[24]; bool heart; int seconds; uint32_t age; };
enum View { V_IDLE, V_HISTORY, V_SAVER };
static View view = V_IDLE;
static Msg hist[5];                     // newest first
static int hist_n = 0, hist_i = 0;
static uint32_t hist_fetched = 0, view_deadline = 0;

static void set_status(const char *top, const char *bottom, Icon icon = ICON_NONE, const char *foot = NULL,
                       const char *corner = NULL)
{
  if (example_lvgl_lock(-1)) {
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

// The "send a message" screen: where every action returns to.
static void show_idle()
{
  view = V_IDLE;
  last_activity = millis();
  char buf[40];
  if (g_pending_id && g_pending_heart) {
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
  set_status("Tobia Talk", "Device", ICON_HEART, g_set.name.c_str(), version);
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
  if (changed && view != V_HISTORY) show_idle();   // also wakes the screensaver
}

static int fetch_history()
{
  WiFiClientSecure tls;
  HTTPClient http;
  if (!begin_request(http, tls, "/messages?last=5")) return 0;
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
  set_status(m.from, bottom, m.heart ? ICON_HEART : ICON_NONE);
  view_deadline = millis() + 10000;
}

// Hold BOOT to record. Returns mono samples, or -1 if BOOT was released within
// TAP_MS (a tap). Keeps recording until MIN_RECORD_MS even if BOOT is released
// early, and stops at MAX_RECORD_MS.
static int record_message()
{
  static int16_t stereo[512];            // 256 frames of L/R
  size_t n = 0;
  bool announced = false;
  uint32_t t0 = millis();
  while (n + 256 <= MONO_CAP_SAMPLES) {
    uint32_t elapsed = millis() - t0;
    bool down = button_down(BOOT_BUTTON_PIN);
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
  if (view == V_HISTORY) {               // in history, BOOT tap replays the shown message
    Msg &m = hist[hist_i];
    if (!m.heart) play_message(m.id, m.from);
    show_history();
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

static void app_task(void *arg)
{
  if (!settings_load(g_set)) {           // never configured: go straight to setup
    enter_setup();
  }
  set_status("Connecting", "WiFi...");
  wifi_start();
  for (int i = 0; i < 40 && !online(); i++) vTaskDelay(pdMS_TO_TICKS(500));
  uint32_t last_poll = 0, last_blink = 0, pwr_t0 = 0, offline_since = millis();
  bool led = false, pwr_was_down = false, shown_offline = false;
  show_idle();

  for (;;) {
    if (button_down(BOOT_BUTTON_PIN)) {
      int n = record_message();
      if (n < 0) {
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
    if (pwr_down && !pwr_was_down) pwr_t0 = millis();
    if (pwr_down && millis() - pwr_t0 >= SETUP_HOLD_MS) enter_setup();
    if (!pwr_down && pwr_was_down && millis() - pwr_t0 > 30) on_pwr_short();
    pwr_was_down = pwr_down;

    if (g_pending_id && millis() - last_blink > 700) {   // blink LED while a message waits
      led = !led;
      set_led(led);
      last_blink = millis();
    } else if (!g_pending_id && led) {
      led = false;
      set_led(false);
    }

    if (view == V_HISTORY && millis() > view_deadline) show_idle();
    if (view == V_IDLE && !g_pending_id && !shown_offline && millis() - last_activity > SAVER_MS) show_saver();

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

    if (online()) {
      offline_since = millis();
      if (shown_offline) { shown_offline = false; show_idle(); }
    } else if (!shown_offline && millis() - offline_since > 30000) {
      shown_offline = true;
      set_status("No WiFi", "PWR 5s: setup");
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

  foot_label = lv_label_create(src_ui.screen);
  lv_label_set_text(foot_label, "");
  lv_obj_set_pos(foot_label, 14, 168);
  lv_obj_set_size(foot_label, 170, 20);
  lv_obj_set_style_text_font(foot_label, &lv_font_montserrat_14, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_color(foot_label, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_align(foot_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_add_flag(foot_label, LV_OBJ_FLAG_HIDDEN);

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
