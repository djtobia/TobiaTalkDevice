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

Two buttons, **BOOT** and **PWR**. What they do depends on the screen:

| Where | Press | Does |
|---|---|---|
| Send screen | Hold **BOOT** | Records (LED on), at least 5 s, at most 15 s; release to send |
| Send screen | Tap **BOOT** (under 0.4 s) | Sends a heart |
| Send screen | Tap **PWR** | Opens history (last 5 messages from *other* boards, newest first) |
| New message waiting | Tap **PWR** | Plays it (a heart is just acknowledged) |
| History | Tap **PWR** | Next older message (wraps around) |
| History | Tap **BOOT** | Previous (newer) message (wraps around) |
| History | Hold **PWR** 3 s | Replays the shown voice message (the screen says so) |
| Anywhere | **PWR + BOOT** together | History -> send screen; send screen -> screensaver |
| Anywhere | Hold **PWR** 5 s | Wi-Fi / name setup portal (not in history view) |
| Anywhere | Hold **PWR + BOOT** 10 s | Powers off a battery-powered board with "OFF" on the screensaver. On USB it just keeps running |
| Screensaver | Tap either button | Wakes to the send screen (the tap does nothing else) |

Screens and indicators:
- **Send screen** (name, "Hold to talk") is home. After 15 s idle a heart screensaver ("Tobia Talk Device",
  name, firmware version top-right) appears. E-paper keeps the image with no power, so it is also what an
  unplugged board shows.
- **New message:** the send screen shows an envelope (or a heart) with the sender, the LED blinks for 5 s, and on
  the screensaver a small envelope appears bottom-right until it is read. Your own messages are never announced to you.
  New messages appear within ~20 s (poll interval).
- **No Wi-Fi:** about 20 s after Wi-Fi is lost (or right after a failed boot connect) the home screen says
  "No WiFi / Hold PWR 5s / to set up WiFi" and stays that way until Wi-Fi returns.
- **Battery** (top-left, only when a battery has been detected): percentage with a battery symbol; a charging bolt
  while USB is plugged in. See "Battery" below.

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
| `GET /messages?last=N&exclude=Name` | Last N (max 20), oldest first. `exclude` leaves out one sender (a board's own name). |
| `GET /audio?id=<id>` | Raw PCM for a voice message (404 once expired). |

A message looks like `{id, from, type, time, seconds?, text?, age}` (`age` = seconds since sent).
The server keeps the last 50 messages; audio expires after 7 days.

## Cost and limits
Free tier at the time of writing: ~100,000 Worker requests/day and ~100,000 KV reads/day.
One board polling every 20 s uses about 4,300 requests/day, so roughly 20 boards fit.
Change `POLL_EVERY_MS` in `firmware/TobiaTalk/user_app.cpp` to trade latency for quota.
Check current limits on Cloudflare's pricing pages before relying on these numbers.

## Battery

The board has a LiPo charger and a power path (ETA6098). Findings from measuring a 3000 mAh battery:
- The voltage pin (GPIO4, 1:2 divider, ADC1 ch3) reads ~4.17 V on USB with **no** battery, which is the same as a
  **full** battery on USB, so voltage alone cannot tell "no battery" from "full battery". The indicator therefore shows
  only when the board knows it has a battery: the **"A battery is installed" checkbox on the setup page** (pre-ticked on
  re-setup), or automatically once it has proven one (a reading < 4.05 V, a plug/unplug step, or still running after the
  cable came out). That is remembered in flash (namespace `battery`, key `seen`).
- **Charging inflates the reading** (~65 mV) and the board's own draw deflates it. There is no charge-status or USB-detect
  pin on a GPIO (the charger's STAT pin only drives the red charge LED), so the firmware watches for the sharp
  60-70 mV step when USB is plugged/unplugged (everyday noise is < 20 mV), tracks "plugged in / on battery", and
  subtracts the learned offset while plugged in. Loads (speaker, recording, uploads) are ignored for a few seconds.
- Near full the offset fades to zero (charging current tapers and then stops), the charging bolt shows only below
  ~4.15 V, and no step is produced when you unplug a board whose charger has finished: a reading that stays 35+ mV
  under its plugged-in peak for a minute is taken as "unplugged".
- Assumes "plugged in" at boot; a board started from battery reads slightly low until the first plug/unplug.
- The firmware sets the **power-latch pin (GPIO17)** at startup. Without it a battery-only board shuts off when PWR is released.
- Battery debugging: set `BATT_DEBUG` to 1 in `user_app.cpp` to log the voltage every 2 s and keep it in flash
  (send `d` over serial to dump, `c` to clear). Note: **opening the serial port resets the board.**
- The user LED (GPIO3) is **green**, single colour. The charger's red LED is wired to the charger only.
- **Audio power saving (tried, OFF by default):** the audio chip (ES8311) and speaker amplifier sit on their own power
  rail (GPIO42, active low). `AUDIO_POWER_SAVE 1` in `user_app.cpp` powers it only while recording/playing and re-initialises
  the codec each time (~0.25 s). **It made battery life worse, not better:** on a 3000 mAh-labelled battery with always-on
  Wi-Fi, always-on audio drained about 1.7-2.2 %/hour, power-saving about 2.8-3.3 %/hour (estimated from voltage; the
  battery's true capacity is probably less than labelled). Likely a leak into the unpowered codec/amp through pins still
  driven by the processor (PA_CTRL GPIO46, I2S/I2C lines). The default is therefore `AUDIO_POWER_SAVE 0` (audio stays on).
  The switch and teardown code are kept for a retry. Recording limits are counted in captured samples, not clock time,
  because the 0.25 s start-up made a clock-based 5 s minimum fall just under the server's 5 s minimum (HTTP 400).
- **Measuring drain:** set `BATT_DEBUG` (and optionally `AUDIO_POWER_SAVE`) via `-D...` lines in `firmware/TobiaTalk/build_opt.h`.
  The Arduino build tool does NOT notice a changed `build_opt.h` on its own: use `arduino-cli compile --clean` (or edit a
  .cpp) so the flag actually applies, and check the `audio_power_save=` field in the serial log. Log every 30 s, compare
  runs that start from a full charge, and compare in the same voltage band; the log ring holds 900 readings (~7.5 h).

## Maintenance notes
- **Lost or leaked family password:** run `npx wrangler secret put FAMILY_KEY` with a new value,
  put the same value in `secrets.h`, and re-flash every board.
- **Redeploy server after edits:** `cd family-relay && npx wrangler deploy`.
- **Wi-Fi changed at someone's house:** hold PWR for 5 s on their board and redo the portal.
- **Clear old test messages:** `npx wrangler kv key delete --binding MSGS --remote log`. Leave the `seq` key alone:
  it keeps message numbers increasing, and boards remember the last number they saw.
- **A board that is "off" on battery** starts again when USB is plugged in or PWR is pressed.
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
