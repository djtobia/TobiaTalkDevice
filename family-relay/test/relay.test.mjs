import assert from "node:assert/strict";
import worker from "../src/worker.js";

const store = new Map();
const env = {
  FAMILY_KEY: "k",
  MSGS: {
    get: async (k, type) => store.get(k) ?? null,
    put: async (k, v) => void store.set(k, v),
    delete: async (k) => void store.delete(k),
  },
  ADMIN_KEY: "adm",
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

// clearing the log must not make ids restart (boards remember the last id they saw)
const before = (await (await call("/messages")).json()).messages.at(-1).id;
store.delete("log");
const afterClear = await (await call("/send?from=Mom&type=heart", { method: "POST" })).json();
assert.equal(afterClear.id, before + 1);

// history can leave out one sender (a board's own messages)
for (const who of ["Dad", "Mom", "Gran", "Mom"]) await call(`/send?from=${who}&type=heart`, { method: "POST" });
const mine = await (await call("/messages?last=20&exclude=Mom")).json();
assert.ok(mine.messages.length > 0 && mine.messages.every((m) => m.from !== "Mom"));
const theirs = await (await call("/messages?last=20")).json();
assert.ok(theirs.messages.some((m) => m.from === "Mom"));
const lastTwo = await (await call("/messages?last=2&exclude=Mom")).json();   // filter first, then take the last N
assert.equal(lastTwo.messages.length, 2);
assert.ok(lastTwo.messages.every((m) => m.from !== "Mom"));

// ---- firmware updates ----
const admin = (path, init = {}, key = "adm") =>
  worker.fetch(new Request("https://x" + path, { ...init, headers: { "x-admin-key": key } }), env);
const fwbin = (n) => new Uint8Array(n).fill(9);

// the family key must NOT be able to publish firmware, and admin needs its own key
assert.equal((await call("/admin/firmware?version=2", { method: "POST", body: fwbin(200000) })).status, 401);
assert.equal((await admin("/admin/firmware?version=2", { method: "POST", body: fwbin(200000) }, "k")).status, 401);
assert.equal((await admin("/admin/firmware?version=2", { method: "POST", body: fwbin(1000) })).status, 400); // too small
assert.equal((await admin("/admin/firmware?version=0", { method: "POST", body: fwbin(200000) })).status, 400);

// nothing published yet -> no offer
let poll = await (await call("/messages?since=0&v=1&me=Mom")).json();
assert.equal(poll.fw, undefined);

// staged: only Dylan gets version 2
const up = await (await admin("/admin/firmware?version=2&only=Dylan", { method: "POST", body: fwbin(200000) })).json();
assert.deepEqual([up.version, up.size, up.only], [2, 200000, ["Dylan"]]);
poll = await (await call("/messages?since=0&v=1&me=Dylan")).json();
assert.deepEqual(poll.fw, { version: 2, size: 200000 });
assert.equal((await (await call("/messages?since=0&v=1&me=Mom")).json()).fw, undefined);   // not in the stage
assert.equal((await (await call("/messages?since=0&v=2&me=Dylan")).json()).fw, undefined); // already current
assert.equal((await (await call("/messages?since=0&me=Dylan")).json()).fw, undefined);     // old firmware reports no version

// widen to everyone
await admin("/admin/rollout?only=all", { method: "POST" });
assert.deepEqual((await (await call("/messages?since=0&v=1&me=Mom")).json()).fw, { version: 2, size: 200000 });

// download requires the family key, returns exact bytes
assert.equal((await worker.fetch(new Request("https://x/firmware?version=2"), env)).status, 401);
const dl = await call("/firmware?version=2");
assert.equal((await dl.arrayBuffer()).byteLength, 200000);
assert.equal((await call("/firmware?version=99")).status, 404);

// versions only move forward unless forced
assert.equal((await admin("/admin/firmware?version=2", { method: "POST", body: fwbin(200000) })).status, 409);
assert.equal((await admin("/admin/firmware?version=2&force=1", { method: "POST", body: fwbin(200000) })).status, 200);

// old versions are cleaned up (keep the newest 3)
for (const v of [3, 4, 5]) await admin(`/admin/firmware?version=${v}`, { method: "POST", body: fwbin(200000) });
assert.deepEqual([2, 3, 4, 5].map((v) => store.has(`fw:${v}`)), [false, true, true, true]);
await admin("/admin/firmware?version=6", { method: "POST", body: fwbin(200000) });
assert.deepEqual([3, 4, 5, 6].map((v) => store.has(`fw:${v}`)), [false, true, true, true]);

console.log("ok");
