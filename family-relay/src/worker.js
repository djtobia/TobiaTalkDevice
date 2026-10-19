// Family voice relay. Boards POST recorded audio (raw 16 kHz, 16-bit, mono PCM)
// or a "heart" (no audio). Every board polls GET /messages?since=<id> for new
// ones, GET /messages?last=N for history, and fetches audio with GET /audio?id=.
// Transcription is intentionally not here yet.
const RATE = 16000;
const BYTES_PER_SEC = RATE * 2;
const MIN_SECONDS = 5;
const MAX_SECONDS = 20;
const MAX_KEPT = 50;
const AUDIO_TTL = 7 * 24 * 3600; // audio expires after a week

const json = (obj, status = 200) =>
  new Response(JSON.stringify(obj), { status, headers: { "content-type": "application/json" } });

// `age` (seconds since sent) lets boards show "5m ago" without knowing the time.
const withAge = (m) => ({ ...m, age: Math.max(0, Math.round((Date.now() - Date.parse(m.time)) / 1000)) });

export default {
  async fetch(req, env) {
    if (req.headers.get("x-family-key") !== env.FAMILY_KEY) return json({ error: "unauthorized" }, 401);
    const url = new URL(req.url);

    if (req.method === "GET" && url.pathname === "/messages") {
      const log = JSON.parse((await env.MSGS.get("log")) || "[]");
      const last = Number(url.searchParams.get("last") || 0);
      if (last > 0) return json({ messages: log.slice(-Math.min(last, 20)).map(withAge) });
      const since = Number(url.searchParams.get("since") || 0);
      return json({ messages: log.filter((m) => m.id > since).map(withAge) });
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
