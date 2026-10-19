# TobiaTalk TODO

Status: server deployed and tested live (`npm test` for the local tests). Firmware runs on two boards
(both V2, non-touch). Both boards currently show v1; the server is offering no firmware.

## Done and tested on hardware
- Voice messages, hearts, history (last 5), envelope/heart icons, screensaver (15 s) with the version tag top-right.
- Wi-Fi/name setup portal (hold PWR 5 s) and per-person names.
- Remote firmware updates (OTA): signed images, separate admin password, staged rollout, automatic rollback,
  rejection of a tampered image. See README "Remote firmware updates". Release tool: `tools/release.mjs`.
- Certificate checking for HTTPS (needs the clock; falls back to unchecked after 3 failures so boards can still be fixed).
- Message ids survive a history clear (server `seq` counter).

## Before shipping boards
- Back up `signing/ota_private.pem` and `signing/admin_key.txt` (they are NOT in git). Also keep the family password.
- Test the "wrong Wi-Fi" path on one board: bad password -> "No WiFi / PWR 5s: setup" -> hold PWR 5 s -> portal.
- Let two boards run for a few days (dropped messages? stuck screen? after a router restart?).
- Clear test messages from the server before the family's boards go online:
  `npx wrangler kv key delete --binding MSGS --remote log` (leave the `seq` key alone).
- Print a one-page setup card for the box (4 steps: plug in, join "TobiaTalk" Wi-Fi, pick Wi-Fi + password + name, Save;
  hold PWR 5 s to redo it).
- 5 more TOUCH boards are coming: check each one's chip first (`esptool chip-id`) and Waveshare's pin differences;
  Waveshare says examples are not interchangeable between hardware versions. The same firmware ignores touch,
  so it should run, but verify before flashing.

## Next
- Verify which TLS mode a board is in (verified vs fallback): add a serial log line.
- Queue of unplayed messages instead of only the newest.
- Several saved Wi-Fi networks per device.
- Setup page niceties: prefill the saved name, on-screen hint to open 192.168.4.1, optional setup-network password.
- Mic channel/gain, speaker volume, LED color/visibility: tune with real use.
- Git: history is clean (no node_modules); latest work is committed locally, push when ready.

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
