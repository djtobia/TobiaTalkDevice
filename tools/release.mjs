#!/usr/bin/env node
// Build, sign and publish a firmware update.
//
//   node tools/release.mjs                      publish a new version to EVERYONE
//   node tools/release.mjs --only Dylan,Autumn  staged: only those boards get it
//   node tools/release.mjs rollout --all        widen the current version to everyone
//   node tools/release.mjs rollout --only Dylan narrow it to named boards
//   node tools/release.mjs status               show what the server is offering
//
// Needs signing/ota_private.pem and signing/admin_key.txt (both git-ignored),
// firmware/TobiaTalk/secrets.h (for the server URL), Arduino IDE (its bundled
// arduino-cli) and Python with the `cryptography` package.
import { spawnSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const sketch = path.join(root, "firmware", "TobiaTalk");
const versionH = path.join(sketch, "version.h");
const buildDir = path.join(root, "build");
const FQBN = "esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=4M,FlashMode=qio,PartitionScheme=min_spiffs,PSRAM=opi";

const die = (msg) => { console.error(`\nERROR: ${msg}`); process.exit(1); };
const need = (p, what) => fs.existsSync(p) || die(`${what} not found at ${p}`);

// ---- config ----
const adminKeyFile = path.join(root, "signing", "admin_key.txt");
const privateKey = path.join(root, "signing", "ota_private.pem");
need(adminKeyFile, "admin key");
const adminKey = fs.readFileSync(adminKeyFile, "utf8").trim();
const secrets = path.join(sketch, "secrets.h");
need(secrets, "secrets.h");
const serverUrl = /#define\s+SERVER_URL\s+"([^"]+)"/.exec(fs.readFileSync(secrets, "utf8"))?.[1] ?? die("SERVER_URL missing in secrets.h");

function findArduinoCli() {
  const candidates = [
    process.env.ARDUINO_CLI,
    "F:/Program Files/ArduinoIDE/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe",
    path.join(process.env.LOCALAPPDATA || "", "Programs/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe"),
    "C:/Program Files/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe",
  ].filter(Boolean);
  return candidates.find((c) => fs.existsSync(c)) ?? "arduino-cli"; // fall back to PATH
}

function findSigningTool() {
  const base = path.join(os.homedir(), "AppData/Local/Arduino15/packages/esp32/hardware/esp32");
  need(base, "ESP32 Arduino core");
  const versions = fs.readdirSync(base).sort();
  return path.join(base, versions.at(-1), "tools", "bin_signing.py");
}

async function admin(pathAndQuery, init = {}) {
  const res = await fetch(serverUrl + pathAndQuery, { ...init, headers: { "x-admin-key": adminKey } });
  const body = await res.json().catch(() => ({}));
  if (!res.ok) die(`${pathAndQuery} -> ${res.status} ${JSON.stringify(body)}`);
  return body;
}

function run(cmd, args, label) {
  console.log(`\n> ${label}`);
  const r = spawnSync(cmd, args, { stdio: "inherit" });
  if (r.status !== 0) throw new Error(`${label} failed`);
}

// ---- args ----
const args = process.argv.slice(2);
const command = ["rollout", "status"].includes(args[0]) ? args.shift() : "release";
const flag = (name) => { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : undefined; };
const only = args.includes("--all") ? "all" : flag("--only") ?? "all";

if (command === "status") {
  console.log(JSON.stringify(await admin("/admin/status"), null, 2));
  process.exit(0);
}

if (command === "rollout") {
  const meta = await admin(`/admin/rollout?only=${encodeURIComponent(only)}`, { method: "POST" });
  console.log(`Version ${meta.version} is now offered to: ${meta.only ? meta.only.join(", ") : "everyone"}`);
  process.exit(0);
}

// ---- release ----
need(privateKey, "private signing key");
const status = await admin("/admin/status");
const current = status.firmware?.version ?? 0;
const localVersion = Number(/FW_VERSION\s+(\d+)/.exec(fs.readFileSync(versionH, "utf8"))[1]);
const next = Number(flag("--version")) || Math.max(current, localVersion) + 1;

const originalVersionH = fs.readFileSync(versionH, "utf8");
try {
  fs.writeFileSync(versionH, originalVersionH.replace(/FW_VERSION\s+\d+/, `FW_VERSION ${next}`));
  fs.mkdirSync(buildDir, { recursive: true });
  run(findArduinoCli(), ["compile", "--fqbn", FQBN, "--output-dir", buildDir, sketch], `Compile v${next}`);
  const bin = path.join(buildDir, "TobiaTalk.ino.bin");
  const signed = path.join(buildDir, `TobiaTalk-v${next}-signed.bin`);
  run("python", [findSigningTool(), "--bin", bin, "--key", privateKey, "--out", signed], "Sign");

  const data = fs.readFileSync(signed);
  console.log(`\n> Upload v${next} (${data.length} bytes) to ${serverUrl}`);
  const meta = await admin(`/admin/firmware?version=${next}&only=${encodeURIComponent(only)}`, { method: "POST", body: data });
  console.log(`\nPublished v${meta.version}. Offered to: ${meta.only ? meta.only.join(", ") : "everyone"}`);
  console.log("Boards pick it up on their next check-in and install it when idle (screensaver).");
  if (meta.only) console.log("Release to everyone with:  node tools/release.mjs rollout --all");
} catch (e) {
  fs.writeFileSync(versionH, originalVersionH); // don't leave a bumped version behind
  die(e.message);
}
