// Family voice relay. Boards POST recorded audio (raw 16 kHz, 16-bit, mono PCM)
// or a "heart" (no audio). Every board polls GET /messages?since=<id> for new
// ones, GET /messages?last=N for history, and fetches audio with GET /audio?id=.
// The same poll tells a board when a newer signed firmware is available.
// Transcription is intentionally not here yet.
const RATE = 16000;
const BYTES_PER_SEC = RATE * 2;
const MIN_SECONDS = 5;
const MAX_SECONDS = 20;
const MAX_KEPT = 50;
const AUDIO_TTL = 7 * 24 * 3600; // audio expires after a week
const MAX_FIRMWARE_BYTES = 2 * 1024 * 1024; // app slot is 1.9 MB + 512 B signature
const MIN_FIRMWARE_BYTES = 100 * 1024;
const KEEP_FIRMWARE_VERSIONS = 3;
const FW_META_CACHE_MS = 30_000;

const json = (obj, status = 200) =>
  new Response(JSON.stringify(obj), { status, headers: { "content-type": "application/json" } });

// `age` (seconds since sent) lets boards show "5m ago" without knowing the time.
const withAge = (m) => ({ ...m, age: Math.max(0, Math.round((Date.now() - Date.parse(m.time)) / 1000)) });

// Firmware metadata is read on every board poll, so cache it briefly per isolate
// to keep KV reads (free tier: 100k/day) from doubling.
let fwCache = { at: 0, meta: null };
async function getFwMeta(env, fresh = false) {
  if (!fresh && Date.now() - fwCache.at < FW_META_CACHE_MS) return fwCache.meta;
  const meta = JSON.parse((await env.MSGS.get("fwmeta")) || "null");
  fwCache = { at: Date.now(), meta };
  return meta;
}
async function putFwMeta(env, meta) {
  await env.MSGS.put("fwmeta", JSON.stringify(meta));
  fwCache = { at: Date.now(), meta };
}

// `only` is a list of device names allowed to update (staged rollout) or null for everyone.
const parseOnly = (s) => {
  const names = (s || "").split(",").map((n) => n.trim()).filter(Boolean);
  return names.length && !names.includes("all") ? names : null;
};

async function handleAdmin(req, env, url) {
  if (!env.ADMIN_KEY || req.headers.get("x-admin-key") !== env.ADMIN_KEY) return json({ error: "unauthorized" }, 401);

  if (req.method === "GET" && url.pathname === "/admin/status") {
    return json({ firmware: await getFwMeta(env, true) });
  }

  if (req.method === "POST" && url.pathname === "/admin/firmware") {
    const version = Number(url.searchParams.get("version"));
    if (!Number.isInteger(version) || version < 1) return json({ error: "bad version" }, 400);
    const bin = await req.arrayBuffer();
    if (bin.byteLength < MIN_FIRMWARE_BYTES || bin.byteLength > MAX_FIRMWARE_BYTES) {
      return json({ error: `firmware must be ${MIN_FIRMWARE_BYTES}-${MAX_FIRMWARE_BYTES} bytes` }, 400);
    }
    const current = await getFwMeta(env, true);
    if (current && version <= current.version && url.searchParams.get("force") !== "1") {
      return json({ error: `version must be greater than ${current.version}` }, 409);
    }
    await env.MSGS.put(`fw:${version}`, bin);
    const meta = { version, size: bin.byteLength, only: parseOnly(url.searchParams.get("only")), time: new Date().toISOString() };
    await putFwMeta(env, meta);
    for (let v = version - KEEP_FIRMWARE_VERSIONS; v >= 1; v--) await env.MSGS.delete(`fw:${v}`);
    return json(meta);
  }

  if (req.method === "POST" && url.pathname === "/admin/rollout") {
    const meta = await getFwMeta(env, true);
    if (!meta) return json({ error: "no firmware published" }, 404);
    meta.only = parseOnly(url.searchParams.get("only"));
    await putFwMeta(env, meta);
    return json(meta);
  }

  return json({ error: "not found" }, 404);
}

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    if (url.pathname.startsWith("/admin/")) return handleAdmin(req, env, url);
    if (req.headers.get("x-family-key") !== env.FAMILY_KEY) return json({ error: "unauthorized" }, 401);

    if (req.method === "GET" && url.pathname === "/messages") {
      const log = JSON.parse((await env.MSGS.get("log")) || "[]");
      const last = Number(url.searchParams.get("last") || 0);
      if (last > 0) return json({ messages: log.slice(-Math.min(last, 20)).map(withAge) });
      const since = Number(url.searchParams.get("since") || 0);
      const out = { messages: log.filter((m) => m.id > since).map(withAge) };
      // Offer a firmware update only to boards that report an older version and are allowed to have it.
      const v = Number(url.searchParams.get("v") || 0);
      const me = url.searchParams.get("me") || "";
      const fw = v > 0 ? await getFwMeta(env) : null;
      if (fw && fw.version > v && (!fw.only || fw.only.includes(me))) out.fw = { version: fw.version, size: fw.size };
      return json(out);
    }

    if (req.method === "GET" && url.pathname === "/firmware") {
      const bin = await env.MSGS.get(`fw:${Number(url.searchParams.get("version"))}`, "arrayBuffer");
      if (!bin) return json({ error: "gone" }, 404);
      return new Response(bin, {
        headers: { "content-type": "application/octet-stream", "content-length": String(bin.byteLength) },
      });
    }

    if (req.method === "GET" && url.pathname === "/audio") {
      const pcm = await env.MSGS.get(`audio:${Number(url.searchParams.get("id"))}`, "arrayBuffer");
      if (!pcm) return json({ error: "gone" }, 404);
      return new Response(pcm, {
        headers: { "content-type": "application/octet-stream", "content-length": String(pcm.byteLength) },
      });
    }

    if (req.method === "POST" && url.pathname === "/send") {
      const from = (url.searchParams.get("from") || "Someone").replace(/[^\w .'-]/g, "").slice(0, 20) || "Someone";
      const type = ["heart", "text"].includes(url.searchParams.get("type")) ? url.searchParams.get("type") : "voice";
      const heart = type === "heart";
      const voice = type === "voice";
      const pcm = await req.arrayBuffer();
      let text = "";
      if (type === "text") {
        text = new TextDecoder().decode(pcm).trim().slice(0, 200);
        if (!text) return json({ error: "empty" }, 400);
      }
      if (voice) {
        if (pcm.byteLength < MIN_SECONDS * BYTES_PER_SEC) return json({ error: `minimum ${MIN_SECONDS}s` }, 400);
        if (pcm.byteLength > MAX_SECONDS * BYTES_PER_SEC) return json({ error: `maximum ${MAX_SECONDS}s` }, 413);
      }
      const log = JSON.parse((await env.MSGS.get("log")) || "[]");
      // `seq` outlives the log, so clearing history never reuses ids (boards remember the last id they saw).
      const id = Math.max(Number(await env.MSGS.get("seq")) || 0, log.at(-1)?.id || 0) + 1;
      await env.MSGS.put("seq", String(id));
      const msg = { id, from, type, time: new Date().toISOString() };
      if (type === "text") msg.text = text;
      if (voice) {
        msg.seconds = Math.round(pcm.byteLength / BYTES_PER_SEC);
        await env.MSGS.put(`audio:${id}`, pcm, { expirationTtl: AUDIO_TTL });
      }
      log.push(msg);
      await env.MSGS.put("log", JSON.stringify(log.slice(-MAX_KEPT)));
      return json(msg);
    }

    return json({ error: "not found" }, 404);
  },
};
