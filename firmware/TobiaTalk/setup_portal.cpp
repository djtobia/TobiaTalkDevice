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

// The drawing pad on the setup page. 80x70 cells, drawn 4x bigger; sends the cells as 1400 hex characters.
static const char EMOJI_JS[] = R"JS(
var W=80,H=70,S=4,bits=new Uint8Array(W*H),c=document.getElementById('cv'),x=c.getContext('2d'),drawn=false,last=null,R=2;
if(cur){for(var i=0;i<W*H;i++){var b=parseInt(cur.substr((i>>3)*2,2),16);bits[i]=(b>>(7-(i&7)))&1;}}
function paint(){x.fillStyle='#fff';x.fillRect(0,0,W*S,H*S);x.fillStyle='#000';for(var i=0;i<W*H;i++)if(bits[i])x.fillRect((i%W)*S,((i/W)|0)*S,S,S);}
function dot(px,py){for(var dy=-R;dy<=R;dy++)for(var dx=-R;dx<=R;dx++){if(dx*dx+dy*dy>R*R+1)continue;var a=px+dx,b=py+dy;if(a>=0&&a<W&&b>=0&&b<H)bits[b*W+a]=1;}}
function pos(e){var r=c.getBoundingClientRect();return [Math.floor((e.clientX-r.left)/r.width*W),Math.floor((e.clientY-r.top)/r.height*H)];}
function line(a,b){var n=Math.max(Math.abs(b[0]-a[0]),Math.abs(b[1]-a[1]),1);for(var i=0;i<=n;i++)dot(Math.round(a[0]+(b[0]-a[0])*i/n),Math.round(a[1]+(b[1]-a[1])*i/n));}
function touched(){drawn=true;var s=document.getElementsByName('icon')[0],has=false;for(var i=0;i<s.options.length;i++)if(s.options[i].value=='custom')has=true;
if(!has){var o=document.createElement('option');o.value='custom';o.text='My own drawing';s.add(o);}s.value='custom';}
var w=document.getElementById('w');
function center(){var r=c.getBoundingClientRect();w.scrollTop=Math.max(1,w.scrollTop+r.top+r.height/2-w.clientHeight/2);}
w.addEventListener('scroll',function(){if(w.scrollTop<1)w.scrollTop=1;});
var centered=false;
function begin(pt){if(!centered){centered=true;center();}if(!drawn){bits.fill(0);}last=pos(pt);touched();dot(last[0],last[1]);paint();}
function move(pt){if(!last)return;var p=pos(pt);line(last,p);last=p;paint();}
function end(){last=null;}
if('ontouchstart' in window){
c.addEventListener('touchstart',function(e){e.preventDefault();begin(e.touches[0]);},{passive:false});
c.addEventListener('touchmove',function(e){e.preventDefault();move(e.touches[0]);},{passive:false});
c.addEventListener('touchend',function(e){e.preventDefault();end();},{passive:false});
c.addEventListener('touchcancel',end);
}else{
c.onmousedown=function(e){e.preventDefault();begin(e);};
window.onmousemove=function(e){move(e);};
window.onmouseup=end;
}
document.addEventListener('touchmove',function(e){if(last)e.preventDefault();},{passive:false});
function pen(r){R=r;var b=document.getElementsByClassName('pen');for(var i=0;i<b.length;i++)b[i].style.background=(i+1==r)?'#222':'#888';}
function clr(){bits.fill(0);drawn=false;document.getElementById('emoji').value='';paint();}
document.forms[0].onsubmit=function(){var h='',any=false;if(drawn){for(var j=0;j<700;j++){var v=0;for(var k=0;k<8;k++){var bit=bits[j*8+k];v=v*2+bit;if(bit)any=true;}h+=(v<16?'0':'')+v.toString(16);}}
document.getElementById('emoji').value=any?h:'';};
pen(2);paint();w.scrollTop=1;
)JS";

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
    uint8_t em[EMOJI_BYTES];
    bool has_em = emoji_load(em);
    String em_hex;                                  // the current drawing, shown again on the canvas
    if (has_em) {
      icons += String("<option value=custom") + (cur.icon == "custom" ? " selected" : "") + ">My own drawing</option>";
      for (int i = 0; i < EMOJI_BYTES; i++) { char b[3]; snprintf(b, sizeof(b), "%02x", em[i]); em_hex += b; }
    }
    String keep = offer_keep ? "<label style='display:block;margin:6px 0 14px'><input type=checkbox name=keepwifi value=1 "
                               "checked style='width:auto;margin:0 8px 0 0'>Keep my current Wi-Fi (" + html_escape(cur.ssid) +
                               "). Untick to pick a different one.</label>"
                             : String("");
    String h =
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<style>html,body{margin:0;height:100%;overflow:hidden;overscroll-behavior:none}"
        "#w{position:fixed;top:0;left:0;right:0;bottom:0;overflow-y:auto;overscroll-behavior:none}"
        "#i{font-family:sans-serif;max-width:420px;margin:0 auto;padding:24px 16px 50vh}"
        "input,select,button{width:100%;font-size:18px;padding:10px;margin:6px 0 14px;box-sizing:border-box}"
        "button{background:#222;color:#fff;border:0;border-radius:8px}</style>"
        "<div id=w><div id=i><h2>TobiaTalk setup</h2><form method=post action=/save>"
        "<label>Your name (shown on the others' screens)</label><input name=name maxlength=15 required value=\"" +
        html_escape(configured ? cur.name : String("")) + "\">"
        "<label>What a quick tap on BOOT sends</label><select name=icon>" + icons + "</select>" + keep +
        "<label>Draw your own quick-send picture (optional)</label>"
        "<canvas id=cv width=320 height=280 style='width:100%;aspect-ratio:8/7;border:2px solid #222;"
        "touch-action:none;display:block;-webkit-user-select:none;user-select:none'></canvas><input type=hidden name=emoji id=emoji>"
        "<div style='display:flex;gap:8px'><button type=button class=pen onclick=pen(1)>Thin</button>"
        "<button type=button class=pen onclick=pen(2)>Medium</button><button type=button class=pen onclick=pen(3)>Thick</button></div>"
        "<button type=button onclick=clr()>Clear the drawing</button>"
        "<p style='margin:0 0 14px;font-size:14px'>Draw with your finger. When you press Save it replaces your old picture, "
        "and it shows up in the picture menu on the device (triple-tap PWR). Others see it as a drawing.</p>"
        "<label>Wi-Fi network</label><select name=ssid>" + options + "</select>"
        "<p><a href=/rescan>List empty or missing yours? Rescan (may pause a few seconds)</a></p>"
        "<label>...or type it if it's not listed</label><input name=ssid_manual>"
        "<label>Wi-Fi password</label><input name=pass type=password>"
        "<button>Save</button></form></div></div><script>var cur='" + em_hex + "';" + String(EMOJI_JS) + "</script>";
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
    String eh = web.arg("emoji");                  // a new drawing (1400 hex characters) replaces the old one
    if (eh.length() == EMOJI_BYTES * 2) {
      uint8_t nb[EMOJI_BYTES];
      bool ok = true, any = false;
      for (int i = 0; i < EMOJI_BYTES && ok; i++) {
        char h2[3] = {eh[i * 2], eh[i * 2 + 1], 0};
        if (!isxdigit((unsigned char)h2[0]) || !isxdigit((unsigned char)h2[1])) ok = false;
        nb[i] = (uint8_t)strtoul(h2, NULL, 16);
        if (nb[i]) any = true;
      }
      if (ok && any) { emoji_save(nb); emoji_set_dirty(true); }
    }
    { uint8_t have[EMOJI_BYTES]; if (s.name != cur.name && emoji_load(have)) emoji_set_dirty(true); }   // server stores it by name
    if (web.arg("icon") == "custom") {             // only if a drawing exists
      uint8_t chk[EMOJI_BYTES];
      if (emoji_load(chk)) s.icon = "custom";
    }
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
