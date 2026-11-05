#include "setup_portal.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "settings.h"

static String html_escape(const String &in)
{
  String out;
  for (char c : in) {
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else out += c;
  }
  return out;
}

static String clean_name(String n)
{
  String out;
  for (char c : n) if (isalnum((unsigned char)c) || c == ' ') out += c;
  out.trim();
  if (out.length() > 15) out = out.substring(0, 15);
  return out.length() ? out : String("Someone");
}

// The quick-send pack, in the order shown on the page. The ids must match QUICK_ICONS in user_app.cpp.
static const char *ICON_CHOICES[][2] = {
    {"heart", "Heart"}, {"smile", "Smiley face"}, {"star", "Star"},
    {"sun", "Sun (good morning)"}, {"moon", "Moon (good night)"}, {"tree", "Christmas tree"}};

void setup_portal_run(const char *ap_name, bool wifi_ok)
{
  Settings cur;
  bool configured = settings_load(cur);            // already set up once: prefill the page and allow keeping Wi-Fi
  bool offer_keep = configured && wifi_ok;

  // A board that is still retrying an old (wrong) network cannot scan, so stop that first.
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(true, false);                    // keeps the saved settings, just stops connecting
  delay(300);
  WiFi.mode(WIFI_AP_STA);

  String options;
  auto scan = [&]() {
    options = "";
    int found = WIFI_SCAN_FAILED;
    for (int attempt = 0; attempt < 3 && found <= 0; attempt++) {
      found = WiFi.scanNetworks();
      if (found <= 0) delay(500);
    }
    for (int i = 0; i < found && i < 20; i++) {
      String s = html_escape(WiFi.SSID(i));
      if (s.length()) options += "<option value=\"" + s + "\">" + s + "</option>";
    }
    WiFi.scanDelete();
  };
  scan();                                          // before the AP starts, so the list is fresh
  WiFi.softAP(ap_name);                            // open network, no password
  IPAddress ip = WiFi.softAPIP();

  DNSServer dns;
  dns.start(53, "*", ip);                          // every hostname -> us, triggers the phone's popup
  WebServer web(80);

  auto page = [&]() {
    String icons;
    for (auto &c : ICON_CHOICES)
      icons += String("<option value=") + c[0] + (cur.icon == c[0] ? " selected" : "") + ">" + c[1] + "</option>";
    String keep = offer_keep ? "<label style='display:block;margin:6px 0 14px'><input type=checkbox name=keepwifi value=1 "
                               "checked style='width:auto;margin:0 8px 0 0'>Keep my current Wi-Fi (" + html_escape(cur.ssid) +
                               "). Untick to pick a different one.</label>"
                             : String("");
    String h =
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<style>body{font-family:sans-serif;max-width:420px;margin:24px auto;padding:0 16px}"
        "input,select,button{width:100%;font-size:18px;padding:10px;margin:6px 0 14px;box-sizing:border-box}"
        "button{background:#222;color:#fff;border:0;border-radius:8px}</style>"
        "<h2>TobiaTalk setup</h2><form method=post action=/save>"
        "<label>Your name (shown on the others' screens)</label><input name=name maxlength=15 required value=\"" +
        html_escape(configured ? cur.name : String("")) + "\">"
        "<label>What a quick tap on BOOT sends</label><select name=icon>" + icons + "</select>" + keep +
        "<label>Wi-Fi network</label><select name=ssid>" + options + "</select>"
        "<p><a href=/rescan>List empty or missing yours? Rescan (may pause a few seconds)</a></p>"
        "<label>...or type it if it's not listed</label><input name=ssid_manual>"
        "<label>Wi-Fi password</label><input name=pass type=password>"
        "<button>Save</button></form>";
    return h;
  };
  web.on("/", HTTP_GET, [&]() { web.send(200, "text/html", page()); });
  web.on("/rescan", HTTP_GET, [&]() {
    scan();
    web.sendHeader("Location", String("http://") + ip.toString() + "/");
    web.send(302, "text/plain", "");
  });
  web.on("/save", HTTP_POST, [&]() {
    Settings s;
    s.ssid = web.arg("ssid_manual").length() ? web.arg("ssid_manual") : web.arg("ssid");
    s.pass = web.arg("pass");
    s.name = clean_name(web.arg("name"));
    s.icon = "heart";                              // anything not in the pack falls back to the heart
    for (auto &c : ICON_CHOICES) if (web.arg("icon") == c[0]) s.icon = c[0];
    if (offer_keep && web.arg("keepwifi") == "1") { s.ssid = cur.ssid; s.pass = cur.pass; }
    if (s.ssid.length() == 0) { web.send(400, "text/plain", "Pick a network"); return; }
    settings_save(s);
    web.send(200, "text/html",
             "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
             "<body style='font-family:sans-serif;text-align:center;margin-top:60px'>"
             "<h2>Saved!</h2><p>The device is restarting and will join your Wi-Fi.</p>");
    delay(1500);
    ESP.restart();
  });
  web.onNotFound([&]() {                           // captive-portal probes land here
    web.sendHeader("Location", String("http://") + ip.toString() + "/");
    web.send(302, "text/plain", "");
  });
  web.begin();

  uint32_t t0 = millis();
  while (millis() - t0 < 10UL * 60 * 1000) {
    dns.processNextRequest();
    web.handleClient();
    delay(5);
  }
  ESP.restart();
}
