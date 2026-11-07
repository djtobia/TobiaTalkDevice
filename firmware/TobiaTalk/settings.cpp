#include "settings.h"
#include <Preferences.h>

bool settings_load(Settings &s)
{
  Preferences p;
  p.begin("tobiatalk", true);
  s.ssid = p.getString("ssid", "");
  s.pass = p.getString("pass", "");
  s.name = p.getString("name", "");
  s.icon = p.getString("icon", "heart");
  p.end();
  if (s.name.length() == 0) s.name = "Someone";
  return s.ssid.length() > 0;
}

void settings_save(const Settings &s)
{
  Preferences p;
  p.begin("tobiatalk", false);
  p.putString("ssid", s.ssid);
  p.putString("pass", s.pass);
  p.putString("name", s.name);
  p.putString("icon", s.icon);
  p.end();
}

bool emoji_load(uint8_t *out)
{
  Preferences p;
  p.begin("tobiatalk", true);
  bool ok = p.getBytes("emoji", out, EMOJI_BYTES) == EMOJI_BYTES;
  p.end();
  return ok;
}

void emoji_save(const uint8_t *in)
{
  Preferences p;
  p.begin("tobiatalk", false);
  p.putBytes("emoji", in, EMOJI_BYTES);
  p.end();
}

bool emoji_dirty()
{
  Preferences p;
  p.begin("tobiatalk", true);
  bool d = p.getBool("emojidirty", false);
  p.end();
  return d;
}

void emoji_set_dirty(bool d)
{
  Preferences p;
  p.begin("tobiatalk", false);
  p.putBool("emojidirty", d);
  p.end();
}
