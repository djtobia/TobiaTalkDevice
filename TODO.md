# TobiaTalk TODO

Status: server tested locally (`npm test` in family-relay). Firmware compiles
(see firmware/README.md for build settings) but has NOT run on a board yet.

## Done (untested on hardware)
- Per-person names, stored on the device, set in the setup portal.
- Wi-Fi setup portal (first boot, or hold PWR 5 s).
- History of the last 5 messages (PWR tap), replay with BOOT tap.
- Heart: tap BOOT.

## Next
- First hardware bring-up: confirm display text fits, mic channel/gain, speaker volume,
  button feel (tap vs hold threshold TAP_MS=400), LED color/visibility.
- Deploy the Worker (Cloudflare account, KV namespace, FAMILY_KEY secret).
- Verify TLS certificate (currently `setInsecure()`).
- Queue of unplayed messages instead of only the newest.
- Several saved Wi-Fi networks per device.
- Confirm V1 vs V2 board (PSRAM setting differs).
- Later: transcription (Whisper) so text shows on screen.
