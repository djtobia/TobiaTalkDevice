# TobiaTalk firmware (Arduino)

Based on Waveshare's `08_Audio_Test` example for the ESP32-S3-ePaper-1.54
(github.com/waveshareteam/ESP32-S3-ePaper-1.54), keeping its display, codec and
power code. Compiles; NOT yet run on hardware.

## Build
1. arduino-esp32 core >= 3.3.0. Libraries: LVGL 8.3.11 + Waveshare's `lv_conf.h`
   (from their repo's `01_Arduino_Libraries/lvgl8`, placed in Documents/Arduino/libraries),
   ArduinoJson 7.x.
2. Board: ESP32S3 Dev Module, USB CDC On Boot enabled, Flash 4MB QIO 80MHz,
   Partition "Huge APP", PSRAM "OPI" (V2 board) or "QSPI" (V1 board).
   CLI: `arduino-cli compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=4M,FlashMode=qio,PartitionScheme=huge_app,PSRAM=opi TobiaTalk`
3. Copy `TobiaTalk/secrets.h.example` to `TobiaTalk/secrets.h`: server URL + family key
   (identical on every device). Wi-Fi and names are NOT in the code.

## First boot / Wi-Fi + name setup (no reflashing)
A new device opens a Wi-Fi network called **TobiaTalk**. Join it on a phone; the setup
page pops up (or browse to 192.168.4.1). Pick the home Wi-Fi, enter its password and
your name, Save. To redo it later, hold **PWR for 5 seconds**.

## Using it
- **Hold BOOT**: record (LED on). At least 5 s, at most 15 s. Release to send.
- **Tap BOOT**: send a heart (no audio).
- **New message** from someone else: the screen shows it and the LED blinks.
  **Tap PWR** to play it (a heart is just acknowledged).
- **Tap PWR with nothing waiting**: browse the last 5 messages (newest first, with
  sender and age). Tap PWR for the next one, tap BOOT to replay it. Leaves after 10 s idle.
- Your own messages are not announced to you.

## Known gaps
- TLS certificate is not verified yet (see TODO in user_app.cpp).
- Only the newest unplayed message is announced; a second one replaces the first.
- Mic channel and gain are untested (stereo is averaged to mono).
- Only one saved Wi-Fi network per device.
