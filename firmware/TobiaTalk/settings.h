// Per-device settings (Wi-Fi + name), stored in flash so nobody has to reflash.
#pragma once
#include <Arduino.h>

struct Settings {
  String ssid, pass, name;
  String icon = "heart";                    // what a tap on BOOT sends (see QUICK_ICONS in user_app.cpp)
};

// The user's own quick-send picture, drawn on the setup page: 80x70, 1 bit per pixel, row-major, MSB first, 1 = black.
#define EMOJI_W 80
#define EMOJI_H 70
#define EMOJI_BYTES (EMOJI_W * EMOJI_H / 8)    // 700
bool emoji_load(uint8_t *out);                  // false if none has been drawn yet
void emoji_save(const uint8_t *in);             // overwrites the old one
bool emoji_dirty();                             // true while the server doesn't have the current picture yet
void emoji_set_dirty(bool d);

bool settings_load(Settings &s);          // false if no Wi-Fi has been configured yet
void settings_save(const Settings &s);
