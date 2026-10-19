#include "setup_portal.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
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

void setup_portal_run(const char *ap_name)
{
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
    String h =
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<style>body{font-family:sans-serif;max-width:420px;margin:24px auto;padding:0 16px}"
        "input,select,button{width:100%;font-size:18px;padding:10px;margin:6px 0 14px;box-sizing:border-box}"
        "button{background:#222;color:#fff;border:0;border-radius:8px}</style>"
        "<h2>TobiaTalk setup</h2><form method=post action=/save>"
        "<label>Your name (shown on the others' screens)</label><input name=name maxlength=15 required>"
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
