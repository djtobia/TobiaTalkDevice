# TobiaTalk TODO

Status: server deployed and tested live (`npm test` for the local tests). Firmware runs on two boards
(both V2, non-touch; one has a 3000 mAh battery). Both show v1; the server is offering no firmware.

## Done and tested on hardware
- Voice messages, hearts, envelope/heart icons, LED blink (5 s) on a new message.
- Screensaver (15 s) with version tag, unread envelope and OFF tag; PWR+BOOT shortcuts; 10 s power-off.
- History of the last 5 messages from other boards, paged with PWR (down) / BOOT (up), 3 s PWR hold to replay.
- Wi-Fi/name setup portal (hold PWR 5 s) incl. network scan after a failed connect + Rescan link.
- Persistent "No WiFi" home screen.
- Battery indicator with plug/unplug tracking and charge offset correction; power latch for battery operation.
- Remote firmware updates (OTA): signed images, separate admin password, staged rollout, automatic rollback,
  rejection of a tampered image. See README. Release tool: `tools/release.mjs`.
- Certificate checking for HTTPS (needs the clock; falls back to unchecked after 3 failures).
- Message ids survive a history clear (server `seq` counter).

## Before shipping boards
- ~~Back up `signing/ota_private.pem`, `signing/admin_key.txt` and the family password~~ (done; they are NOT in git).
- Overnight/multi-day run with two boards side by side (in progress): dropped messages? stuck screen? Wi-Fi recovery?
- Test the "wrong Wi-Fi" path once more on the final build (done on an earlier build): bad password -> "No WiFi" ->
  hold PWR 5 s -> portal -> fix.
- Clear test messages from the server before the family's boards go online:
  `npx wrangler kv key delete --binding MSGS --remote log` (leave the `seq` key alone).
- Print a one-page setup card for the box (4 steps: plug in, join "TobiaTalk" Wi-Fi, pick Wi-Fi + password + name, Save;
  hold PWR 5 s to redo it).
- 5 more TOUCH boards are coming: check each one's chip first (`esptool chip-id`) and Waveshare's pin differences;
  Waveshare says examples are not interchangeable between hardware versions. The same firmware ignores touch,
  so it should run, but verify before flashing.
- Flash every board over USB once with the final firmware at v1 (the version tag shows in the screensaver corner).

## Next
- **When the battery-equipped boards arrive:** every board will have a battery, so drop the "A battery is installed" checkbox
  from the setup page and make "battery present" the built-in default (e.g. a `BATTERY_INSTALLED 1` constant in
  `user_app.cpp`; keep the plug/unplug tracking). Consequence: a board with NO battery would then show a bogus ~100%
  (USB with no battery reads ~4.17 V, the same as a full battery), so the two current test boards need either a
  battery too or a one-off build with the constant set to 0.
- Verify which TLS mode a board is in (verified vs fallback): add a serial log line.
- Queue of unplayed messages instead of only the newest.
- Several saved Wi-Fi networks per device.
- Setup page niceties: prefill the saved name, on-screen hint to open 192.168.4.1, optional setup-network password.
- Mic channel/gain, speaker volume: tune with real use.
- Battery: unknown plug state at boot when started from battery (reads slightly low until the first plug/unplug).

## Ideas (not started)
- **Text display + speech-to-text.** Show the `text` field on screen (smaller font, paging for
  long text); transcribe voice on the server in the background (OpenAI or Cloudflare Workers AI;
  check current pricing). Falls back to audio-only if transcription fails. The server already has a `text` type.
- **Typing alternatives for people who don't want to speak:**
  - Canned replies ("Love you", "On my way", "Call me", "Good night"): PWR scrolls, BOOT sends, goes
    out as a `text` message. Phrase list could come from the setup page or the server.
  - Multi-tap shortcuts (1 tap heart, 2 taps "thinking of you", 3 taps "call me").
  - Slower options if wanted: button-based letter picker, Morse with one button.
- **Touch version only: drawn images.** Touch drawing into a 200x200 1-bit bitmap (5 KB), sent as a
  new `drawing` message type (stored like audio, size-checked on the server). Receivers draw it
  full-screen; non-touch boards can still receive. E-paper refresh is slow, so expect chunky
  line updates. Needs a separate pixel-level drawing mode (current display path redraws the
  whole screen each update). Cheaper alternative that works on any board: preset "stickers"
  (small pictures like a flower or a cake) sent like canned replies.
- SD card (the board has a TF slot): possible local message log or sounds later.
