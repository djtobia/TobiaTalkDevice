#include "settings.h"
#include <Preferences.h>

bool settings_load(Settings &s)
{
  Preferences p;
  p.begin("tobiatalk", true);
  s.ssid = p.getString("ssid", "");
  s.pass = p.getString("pass", "");
  s.name = p.getString("name", "");
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
  p.end();
}
