import assert from "node:assert/strict";
import worker from "../src/worker.js";

const store = new Map();
const env = {
  FAMILY_KEY: "k",
  MSGS: {
    get: async (k, type) => store.get(k) ?? null,
    put: async (k, v) => void store.set(k, v),
  },
};
const call = (path, init = {}, key = "k") =>
  worker.fetch(new Request("https://x" + path, { ...init, headers: { "x-family-key": key } }), env);
const pcm = (sec) => new Uint8Array(sec * 32000).fill(7);

assert.equal((await call("/messages", {}, "bad")).status, 401);
assert.equal((await call("/send?from=Mom", { method: "POST", body: pcm(4) })).status, 400);
assert.equal((await call("/send?from=Mom", { method: "POST", body: pcm(21) })).status, 413);

const m1 = await (await call("/send?from=Mom", { method: "POST", body: pcm(6) })).json();
assert.deepEqual([m1.id, m1.from, m1.seconds, m1.type], [1, "Mom", 6, "voice"]);
await call("/send?from=Dad%3Cb%3E", { method: "POST", body: pcm(5) });

// heart: no audio needed, bypasses the 5 s minimum
const h = await (await call("/send?from=Gran&type=heart", { method: "POST" })).json();
assert.deepEqual([h.id, h.type, h.seconds], [3, "heart", undefined]);
assert.equal((await call("/audio?id=3")).status, 404);

const all = await (await call("/messages")).json();
assert.deepEqual(all.messages.map((m) => m.from), ["Mom", "Dadb", "Gran"]);
assert.equal(typeof all.messages[0].age, "number");
assert.equal((await (await call("/messages?since=1")).json()).messages.length, 2);

// history: last N, oldest first
const hist = await (await call("/messages?last=2")).json();
assert.deepEqual(hist.messages.map((m) => m.id), [2, 3]);

const audio = await call("/audio?id=1");
assert.equal((await audio.arrayBuffer()).byteLength, 6 * 32000);
assert.equal((await call("/audio?id=99")).status, 404);
// text: stored in the log, trimmed and capped at 200 chars, no audio, empty rejected
const t = await (await call("/send?from=Claude&type=text", { method: "POST", body: "  Hi from Claude \n" })).json();
assert.deepEqual([t.id, t.type, t.text, t.seconds], [4, "text", "Hi from Claude", undefined]);
assert.equal((await call("/audio?id=4")).status, 404);
assert.equal((await call("/send?from=Claude&type=text", { method: "POST", body: "   " })).status, 400);
const long = await (await call("/send?from=Claude&type=text", { method: "POST", body: "x".repeat(500) })).json();
assert.equal(long.text.length, 200);
assert.equal((await (await call("/messages?since=3")).json()).messages[0].text, "Hi from Claude");

console.log("ok");
