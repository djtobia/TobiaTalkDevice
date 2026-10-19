# TobiaTalk TODO

Status: server deployed and tested live (`npm test` for the local tests). Firmware
compiles (see firmware/README.md for build settings) but has NOT run on a board yet.

## Done
- Server deployed: https://family-relay.djtobia.workers.dev (voice, heart, text types).
- Per-person names, stored on the device, set in the setup portal (untested on hardware).
- Wi-Fi setup portal: first boot, or hold PWR 5 s (untested on hardware).
- History of the last 5 messages (PWR tap), replay with BOOT tap (untested on hardware).
- Heart: tap BOOT. Tested on two boards: heart and envelope icons look good.
- Screensaver (15 s idle, heart + "Tobia Talk Device") and envelope icon for new messages; tap wakes to the send screen.
- Firmware built with the two-slot "Minimal SPIFFS" partition scheme so OTA updates are possible later.

## Next
- First hardware bring-up: confirm display text fits, mic channel/gain, speaker volume,
  button feel (tap vs hold threshold TAP_MS=400), LED color/visibility.
  The two test messages already on the server ("Hi from Claude" text, a heart) should show in
  the history view as a first check; the text one will look wrong until text display exists.
  Delete them afterwards: `npx wrangler kv key delete --binding MSGS --remote log`.
- First two boards are V2 (8 MB flash/8 MB PSRAM), NOT touch. 5 more TOUCH boards are coming: check their chip
  (esptool chip-id) and Waveshare's pin differences before flashing; Waveshare says examples are not
  interchangeable between hardware versions. The same firmware ignores touch, so it should run, but verify.
- Verify TLS certificate (currently `setInsecure()`).
- Queue of unplayed messages instead of only the newest.
- Several saved Wi-Fi networks per device.
- Git: single clean history is pending (staged on branch `clean`, then force-push); README and
  doc edits not committed yet.

## Ideas (not started)
- **Text display + speech-to-text.** Show the `text` field on screen (smaller font, paging for
  long text); transcribe voice on the server in the background (OpenAI or Cloudflare Workers AI;
  check current pricing). Falls back to audio-only if transcription fails.
- **Remote firmware updates (OTA).** Boards pull a signed `firmware.bin` from the Worker. Needs:
  separate admin upload key, signature check, real TLS verification first, rollback if the new
  version can't reach the server, staged rollout (one board first), update only when idle/plugged in.
  Do NOT host the binary publicly: `secrets.h` (family password) is compiled into it.
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
