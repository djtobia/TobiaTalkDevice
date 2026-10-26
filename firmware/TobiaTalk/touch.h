// FT6336 capacitive touch controller (touch versions of the Waveshare ESP32-S3-ePaper-1.54 only).
// Non-touch boards have no chip at I2C address 0x38, so touch_init() simply returns false there.
#pragma once
#include <stdint.h>

// Call after the audio codec has been initialised (they share the I2C bus on GPIO47/48).
// Returns true if a touch controller answered.
bool touch_init();

// Reads the current touch. Returns true and fills x, y (0..199, raw controller orientation) while a finger is down.
bool touch_read(uint16_t *x, uint16_t *y);
