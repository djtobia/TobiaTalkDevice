// Per-device settings (Wi-Fi + name), stored in flash so nobody has to reflash.
#pragma once
#include <Arduino.h>

struct Settings {
  String ssid, pass, name;
};

bool settings_load(Settings &s);          // false if no Wi-Fi has been configured yet
void settings_save(const Settings &s);
