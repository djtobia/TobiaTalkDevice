# TobiaTalk firmware (Arduino)

Based on Waveshare's `08_Audio_Test` example for the ESP32-S3-ePaper-1.54
(github.com/waveshareteam/ESP32-S3-ePaper-1.54), keeping its display, codec and
power code. Runs on hardware (two V2 boards tested). See the **root README** for
what the buttons do, how updates work and how to set a board up; this file is
just the firmware build notes.

## Build
1. arduino-esp32 core >= 3.3.0 (3.3.12 tested). Libraries: LVGL 8.3.11 + Waveshare's `lv_conf.h`
   (from their repo's `01_Arduino_Libraries/lvgl8`, placed in Documents/Arduino/libraries),
   ArduinoJson 7.x.
2. Board: ESP32S3 Dev Module, USB CDC On Boot enabled, Flash 4MB QIO 80MHz,
   Partition "Minimal SPIFFS (1.9MB APP with OTA)", PSRAM "OPI" (V2 board) or "QSPI" (V1 board).
   CLI: `arduino-cli compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=4M,FlashMode=qio,PartitionScheme=min_spiffs,PSRAM=opi TobiaTalk`
3. Copy `TobiaTalk/secrets.h.example` to `TobiaTalk/secrets.h`: server URL + family key
   (identical on every device). Wi-Fi and names are NOT in the code; they are set on the device.
4. `build_opt.h` (`-DUPDATE_SIGN`) turns on signed-update checking; `ota_public.h` holds the public key.
   Versions are in `version.h` (bumped by `tools/release.mjs`).

## Files
| File | What |
|---|---|
| `TobiaTalk.ino` | LVGL/e-paper setup from Waveshare's example |
| `user_app.cpp` | App logic: buttons, screens, polling, recording/playback, battery, updates |
| `settings.*` | Wi-Fi + name saved in flash |
| `setup_portal.*` | "TobiaTalk" captive-portal setup page |
| `ota.*` | Signed over-the-air update install, rollback |
| `src/`, `audio_bsp.*` | Waveshare's display / codec / power / button drivers |

## Notes
- Opening the board's USB serial port resets it.
- Firmware is ~1.46 MB of a 1.97 MB app slot.
