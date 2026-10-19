# TobiaTalkDevice

A family "walkie-talkie" gift: small e-paper boxes that send voice messages and
hearts to each other over the internet. Hold a button and talk; every other box
shows "New message from Mom" and plays it when you press a button.

Built on the **Waveshare ESP32-S3-ePaper-1.54** (200x200 black/white e-paper,
built-in microphone, speaker, two buttons, Wi-Fi) and a tiny **Cloudflare Worker**
that relays messages. No app, no phone needed for the family.

```
 board (hold BOOT, talk) --HTTPS POST--> Cloudflare Worker + KV <--HTTPS poll (every 20 s)-- other boards
```

Boards cannot talk to each other directly (home routers block that), so every board
only makes outgoing HTTPS requests to the Worker, which stores messages for a while.

## Repo layout

| Path | What |
|---|---|
| `family-relay/` | The server: one Cloudflare Worker (`src/worker.js`), tests, deploy config |
| `firmware/TobiaTalk/` | The board firmware (Arduino sketch) |
| `firmware/README.md` | Firmware build/usage details |
| `tools/release.mjs` | Builds, signs and publishes a firmware update (see "Remote firmware updates") |
| `signing/` | **Git-ignored, local only:** `ota_private.pem` (signing key) and `admin_key.txt` (admin password). Back these up! |
| `TODO.md` | What is done and what is left |

## How it behaves

- **Hold BOOT**: records (LED on). Always at least 5 s, at most 15 s. Release to send.
- **Tap BOOT** (under 0.4 s): sends a heart.
- **New message** from someone else: screen shows it, LED blinks. **Tap PWR** to play it.
- **Tap PWR** with nothing waiting: browse the last 5 messages. PWR = next, BOOT = replay. Leaves after 10 s.
- **Hold PWR 5 s**: opens the Wi-Fi/name setup portal.
- **Screens**: the send screen (name, "Hold to talk") is home. After 15 s idle a heart screensaver ("Tobia Talk Device") appears and stays on the e-paper even when unplugged. Any tap wakes it (the tap does nothing else). A waiting voice message shows an envelope icon.
- Your own messages are never announced to you. New messages appear within ~20 s (poll interval).

## Setting everything up from scratch

### 0. What you need
- 2+ Waveshare ESP32-S3-ePaper-1.54 boards + USB-C cables. Check which hardware version you
  have, **V1 (2 MB PSRAM) or V2 (8 MB PSRAM)**; Waveshare says their examples are not interchangeable,
  and it changes one Arduino setting (below). See https://docs.waveshare.com/ESP32-S3-ePaper-1.54
- A free Cloudflare account (https://dash.cloudflare.com). No card needed at family scale.
- Node.js (any recent LTS) and Arduino IDE 2.x.

### 1. Deploy the server (Cloudflare Worker)
```bash
cd family-relay
npm install
npx wrangler login                          # opens the browser, click Allow
npx wrangler kv namespace create MSGS       # prints an id
```
Paste that id into `wrangler.toml` (`id = "..."`). Then:
```bash
npx wrangler deploy                         # prints https://family-relay.<you>.workers.dev
```
Make a random family password and store it as a server secret:
```bash
node -e "console.log(require('crypto').randomBytes(24).toString('base64url'))"
npx wrangler secret put FAMILY_KEY          # paste the value when asked
```
Keep that value; the boards need the same one (step 2). A brand-new `workers.dev` address can
take a couple of minutes before HTTPS works.

Smoke test (replace URL and KEY):
```bash
curl -X POST -H "x-family-key: KEY" "https://family-relay.<you>.workers.dev/send?from=Test&type=heart"
curl -H "x-family-key: KEY" "https://family-relay.<you>.workers.dev/messages?last=5"
```
Run the unit tests any time with `npm test` (no network needed).

To look at stored messages: Cloudflare dashboard -> Storage & Databases -> KV -> MSGS ->
KV Pairs -> `log` (readable list). `audio:<id>` entries are raw audio and expire after 7 days.
Or: `npx wrangler kv key get --binding MSGS --remote log`.

### 2. Build and flash the firmware
1. Arduino IDE 2.x. Add the ESP32 board URL
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json` and install the
   **esp32 by Espressif** core, version **3.3.x** (built/tested with 3.3.12; Waveshare needs >= 3.3.0).
2. Libraries: **ArduinoJson 7.x** (Library Manager) and **LVGL 8.3.11**.
   LVGL needs a config file: copy `lv_conf.h` from Waveshare's repo
   (`01_Arduino_Libraries/lvgl8/lv_conf.h` at https://github.com/waveshareteam/ESP32-S3-ePaper-1.54)
   into your Arduino `libraries/` folder (next to, not inside, the `lvgl` folder).
3. Copy `firmware/TobiaTalk/secrets.h.example` to `firmware/TobiaTalk/secrets.h` and fill in
   `SERVER_URL` and `FAMILY_KEY` (same on every board). This file is git-ignored on purpose.
4. Open `firmware/TobiaTalk/TobiaTalk.ino`. Tools menu settings:
   - Board: **ESP32S3 Dev Module**
   - USB CDC On Boot: **Enabled**
   - Flash Mode **QIO 80MHz**, Flash Size **4MB**
   - Partition Scheme: **Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)** (two app slots, needed for over-the-air updates later)
   - PSRAM: **OPI PSRAM** for V2, **QSPI PSRAM** for V1
5. Plug in the board, pick its port, Upload. If upload fails, hold BOOT while plugging in
   the USB cable (download mode), then retry.

Command-line equivalent (arduino-cli ships inside Arduino IDE):
```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=4M,FlashMode=qio,PartitionScheme=min_spiffs,PSRAM=opi firmware/TobiaTalk
```

### 3. Set up each board (no reflashing per household)
Flash the same firmware on every board. On first power-up (or after holding PWR 5 s) the board
opens a Wi-Fi network named **TobiaTalk**. On a phone, join it; the setup page pops up (or open
`192.168.4.1`). Choose the home Wi-Fi, enter its password and the person's name (letters, digits,
spaces, up to 15), Save. The board restarts and joins. Do this at the recipient's house, or
ahead of time with their Wi-Fi details.

## Remote firmware updates (OTA)

Boards update themselves: every board reports its firmware version in its normal check-in; if the
server offers a newer one the board downloads it, checks the **signature**, installs it and restarts.
This happens only when the board is idle on the screensaver, has no message waiting and is online.
The version shows in the top-right corner of the screensaver (e.g. `v1`).

Safety layers, all tested on real boards:
- **Signed images.** Boards only accept firmware signed with your private key (ECDSA P-256, SHA-256);
  the public key is compiled in (`firmware/TobiaTalk/ota_public.h`). A tampered file is refused
  ("Update failed - Signature Verification Failed") and the board keeps running.
- **Separate admin password.** Publishing needs `x-admin-key`; the family password cannot publish.
- **Automatic rollback.** A newly installed image must reach the server within 3 minutes (or not crash);
  otherwise the bootloader returns to the previous image. Tested with a deliberately crashing build.
- **Staged rollout.** Publish to named boards first, then widen.
- **Retry rules.** A version that fails to install is not retried for an hour; a different version is tried at once.

### One-time setup (already done for this project)
```bash
pip install cryptography                     # used by the signing tool
python <esp32-core>/tools/bin_signing.py --generate-key ecdsa-p256 --out signing/ota_private.pem
python <esp32-core>/tools/bin_signing.py --extract-pubkey signing/ota_private.pem --out firmware/TobiaTalk/ota_public.pem
# generate an admin password, save it to signing/admin_key.txt, and set it on the server:
cd family-relay && npx wrangler secret put ADMIN_KEY && npx wrangler deploy
```
`firmware/TobiaTalk/build_opt.h` (`-DUPDATE_SIGN`) turns on signature checking in the build. The
firmware must be flashed over USB **once** with the public key inside; after that updates are remote.
If you ever lose `signing/ota_private.pem`, boards can no longer be updated remotely: make a new key
and re-flash every board over USB.

### Publishing an update
```bash
node tools/release.mjs --only Dylan      # build, sign, publish to just the named board(s) (staged)
# ... watch that board update itself and look fine, then:
node tools/release.mjs rollout --all     # offer it to everyone
node tools/release.mjs                   # (or publish straight to everyone)
node tools/release.mjs status            # what the server is offering
```
The tool bumps `FW_VERSION` in `firmware/TobiaTalk/version.h` itself. Versions only go up on the server
(it refuses a lower number). To reset the numbering during testing, delete the stored versions:
`npx wrangler kv key delete --binding MSGS --remote fwmeta` (and `fw:<n>`), and set `FW_VERSION` back
in `version.h` before flashing boards over USB.

Do not host firmware binaries anywhere public: `secrets.h` (the family password) is compiled into them.

## Server API (all requests need header `x-family-key: <FAMILY_KEY>`, except `/admin/*` which needs `x-admin-key`)

| Request | Purpose |
|---|---|
| `POST /send?from=Name` body = raw PCM | Voice message. 16 kHz, 16-bit little-endian, mono. 5-20 s (else 400/413). |
| `POST /send?from=Name&type=heart` | Heart, no body. |
| `POST /send?from=Name&type=text` body = UTF-8 text | Text (max 200 chars). **Firmware does not display this yet.** |
| `GET /messages?since=<id>&v=<fwversion>&me=<Name>` | Messages newer than id (what boards poll). If a newer firmware is on offer for that board the reply also has `fw: {version, size}`. |
| `GET /firmware?version=<n>` | The signed firmware image. |
| `POST /admin/firmware?version=<n>&only=A,B` | Publish a signed image (admin key). `only` omitted/`all` = everyone. Versions must increase (`force=1` overrides). |
| `POST /admin/rollout?only=A,B` or `only=all` | Change who is offered the current version (admin key). |
| `GET /admin/status` | Current firmware offer (admin key). |
| `GET /messages?last=N` | Last N (max 20), oldest first. |
| `GET /audio?id=<id>` | Raw PCM for a voice message (404 once expired). |

A message looks like `{id, from, type, time, seconds?, text?, age}` (`age` = seconds since sent).
The server keeps the last 50 messages; audio expires after 7 days.

## Cost and limits
Free tier at the time of writing: ~100,000 Worker requests/day and ~100,000 KV reads/day.
One board polling every 20 s uses about 4,300 requests/day, so roughly 20 boards fit.
Change `POLL_EVERY_MS` in `firmware/TobiaTalk/user_app.cpp` to trade latency for quota.
Check current limits on Cloudflare's pricing pages before relying on these numbers.

## Maintenance notes
- **Lost or leaked family password:** run `npx wrangler secret put FAMILY_KEY` with a new value,
  put the same value in `secrets.h`, and re-flash every board.
- **Redeploy server after edits:** `cd family-relay && npx wrangler deploy`.
- **Wi-Fi changed at someone's house:** hold PWR for 5 s on their board and redo the portal.
- **Clear old test messages:** `npx wrangler kv key delete --binding MSGS --remote log`.
- **If a toolchain upgrade breaks the build:** the last known-good versions were arduino-esp32 3.3.12,
  LVGL 8.3.11, ArduinoJson 7.4.3, wrangler 4.x, Node 22.

## Credits and licensing
The display, audio codec, power and button drivers in `firmware/TobiaTalk/src/` and `audio_bsp.*`
come from Waveshare's `08_Audio_Test` Arduino example
(https://github.com/waveshareteam/ESP32-S3-ePaper-1.54); their license files are kept alongside.
The app logic (`user_app.cpp`, `settings.*`, `setup_portal.*`) and the Worker are this project's own.

## Known gaps
See `TODO.md`. Main ones: only the newest unplayed message is announced; one saved Wi-Fi network per
board; the board checks the server's certificate once its clock is set but falls back to an unchecked
connection after 3 failures (firmware downloads are protected by their signature either way).
