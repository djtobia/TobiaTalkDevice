# TobiaTalk TODO

Status: server deployed and tested live (`npm test` for the local tests). Firmware runs on three boards: two
non-touch V2 (one with a 3000 mAh battery, one with none) and one touch V2 with a 400 mAh battery. All show v1; the
server is offering no firmware update.

## Done and tested on hardware
- Voice messages, hearts, **drawings** (touch boards draw; every board receives and views them), envelope/heart icons,
  LED blink (5 s) on a new message.
- Screensaver (15 s) with version tag, unread envelope and OFF tag; PWR+BOOT shortcuts; 10 s power-off.
- History of the last 5 messages from other boards, paged with PWR (down) / BOOT (up), 3 s PWR hold to replay/view.
- Wi-Fi/name setup portal (hold PWR 5 s) incl. network scan after a failed connect + Rescan link.
- Persistent "No WiFi" home screen.
- Battery indicator (on by default now, no setup checkbox) with plug/unplug tracking and charge offset correction;
  power latch for battery operation.
- Adaptive polling: once a minute when quiet, every 10 s for 2 min after activity.
- Remote firmware updates (OTA): signed images, separate admin password, staged rollout, automatic rollback,
  rejection of a tampered image. See README. Release tool: `tools/release.mjs`.
- Certificate checking for HTTPS (needs the clock; falls back to unchecked after 3 failures).
- Message ids survive a history clear (server `seq` counter).
- Backups of `signing/ota_private.pem`, `signing/admin_key.txt` and the family password (NOT in git).
- Touch board: same chip as the first boards (ESP32-S3-PICO-1, 8 MB / 8 MB), same firmware runs unchanged.

## Before shipping boards
- Test the "wrong Wi-Fi" path once more on the final build (done on an earlier build): bad password -> "No WiFi" ->
  hold PWR 5 s -> portal -> fix.
- Check each remaining touch board's chip (`esptool chip-id`) before flashing; flash everything over USB at v1
  (the version tag shows in the screensaver corner) and set each board up with its own name.
- **Decide about the 400 mAh batteries:** at ~50-60 mA average draw that is only ~6-8 hours on battery. Fine as "keeps
  running while moved", but the boards will really need USB power (or a bigger battery).
- Clear test messages from the server (and any test drawings) right before the boards go online:
  `npx wrangler kv key delete --binding MSGS --remote log` (leave the `seq` key alone).
- Print a one-page setup card for the box (plug in, join "TobiaTalk" Wi-Fi, pick Wi-Fi + password + name, Save;
  hold PWR 5 s to redo it; touch to draw, BOOT sends, PWR clears).
- The old non-touch test board has no battery and now shows a bogus ~100% (battery indicator defaults on): flash it
  with `-DBATTERY_INSTALLED=0` (use `arduino-cli compile --clean`) or give it a battery.

## Next
- **Drawing polish (waiting for a stylus):** gentle smoothing of touch points; a pen-size choice (currently a 3 px pen);
  maybe an eraser. A passive capacitive stylus with a ~4-6 mm tip works better than a fingertip. Strokes appear in
  ~0.4 s bursts (the e-paper's redraw time); that is the panel's limit.
- **Battery drain / audio power saving: UNRESOLVED, default is audio always on.** Voltage-log comparisons first suggested
  switching the audio rail off made drain worse, but they were estimated from voltage at different points on the battery
  curve and a later full-range run (always-on firmware, 90% -> 5% in 21 h, ~4 %/h, about 1 day from full) does not fit
  them. Do NOT trust the percentage comparisons. When the USB power meter arrives (ordered): measure real current for
  `AUDIO_POWER_SAVE` 0 vs 1 (a few minutes each), then decide. If power saving really leaks, put the audio chip's signal
  pins (GPIO 14/15/16/38/45 I2S, 46 PA_CTRL, 47/48 I2C) in a harmless state before cutting its power. Other drain levers:
  Wi-Fi light sleep between checks, running the CPU slower, display/Wi-Fi activity. Re-measure battery life with the
  meter to see the gain from adaptive polling. Note: touch needs the I2C bus to stay up, so power saving would also need
  the touch driver to cope with the audio chip's I2C bus being torn down.
- Verify which TLS mode a board is in (verified vs fallback): add a serial log line.
- Queue of unplayed messages instead of only the newest.
- Several saved Wi-Fi networks per device.
- Setup page niceties: prefill the saved name, on-screen hint to open 192.168.4.1, optional setup-network password.
- Mic channel/gain, speaker volume: tune with real use.
- Battery: unknown plug state at boot when started from battery (reads slightly low until the first plug/unplug).
- Accidental touches: a touch on the send screen starts a drawing (BOOT+PWR or 90 s idle leaves it). Watch for boards
  that get picked up and start drawing by themselves; if it is a nuisance, require a deliberate gesture.

## Ideas (not started)
- **Stickers / stamps** (small preset pictures to place by touch, or sent like canned replies): good drawings without
  steady hands, works for kids and grandparents.
- **Networks with a sign-in page (hotel/airport/café/dorm Wi-Fi).** Today a board joins such a network but never
  reaches the server, and the screen shows nothing wrong (the "No WiFi" screen only appears when Wi-Fi itself fails).
  If boards ever need to work there:
  - Add an internet check: Wi-Fi up but the server unreachable -> show "Connected, no internet / network may need sign-in".
  - Add a no-password `/ping` endpoint on the Worker and only send the family key after `/ping` answers correctly. This also
    closes a small hole: after 3 failed certificate checks the board falls back to unverified TLS, and on a sign-in
    network that could send the family key to the network's own login server.
  - Show the board's MAC address (setup page and screen) so it can be pre-registered where networks allow that.
  - Workaround today: a phone's personal hotspot (no sign-in page).
  - Out of scope unless needed: auto-completing sign-in pages (every portal differs) and WPA2-Enterprise networks
    (eduroam-style username + password; the setup page only takes a plain password).
- **Text display + speech-to-text.** Show the `text` field on screen (smaller font, paging for
  long text); transcribe voice on the server in the background (OpenAI or Cloudflare Workers AI;
  check current pricing). Falls back to audio-only if transcription fails. The server already has a `text` type.
- **Typing alternatives for people who don't want to speak:**
  - Canned replies ("Love you", "On my way", "Call me", "Good night"): PWR scrolls, BOOT sends, goes
    out as a `text` message. Phrase list could come from the setup page or the server.
  - Multi-tap shortcuts (1 tap heart, 2 taps "thinking of you", 3 taps "call me").
  - Slower options if wanted: button-based letter picker, Morse with one button.
- SD card (the board has a TF slot): possible local message log or sounds later.
