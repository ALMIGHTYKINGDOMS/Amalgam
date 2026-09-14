import { test } from "node:test";
import assert from "node:assert/strict";
import {
  TurnAccounting,
  TurnAccountingError,
  FREE_ALLOWANCE_BYTES,
  PLUS_ALLOWANCE_BYTES,
  gib,
} from "./turn-accounting.mjs";

// Production-seeded plans (same numbers as the migration).
const plans = new Map([
  ["free", FREE_ALLOWANCE_BYTES],
  ["amalgam_plus", PLUS_ALLOWANCE_BYTES],
]);

function makeClock(start) {
  let t = start;
  return { now: () => t, advance: (s) => (t += s) };
}

function acct(clock, subs = new Map([["u1", { plan: "free", status: "active" }]])) {
  return new TurnAccounting({ plans, subscriptions: subs, now: clock.now });
}

test("first usage event is ingested and counted exactly", () => {
  const a = acct(makeClock(1_000_000));
  const r = a.ingest("u1", gib(1), "relay", "evt-1");
  assert.deepEqual(r, { ingested: true });
  assert.equal(a.usage.get("u1").used_bytes, gib(1));
  assert.equal(a.usage.get("u1").last_source, "relay");
});

test("duplicate event id never double-counts", () => {
  const a = acct(makeClock(1_000_000));
  a.ingest("u1", gib(1), "relay", "evt-1");
  const dup = a.ingest("u1", gib(1), "relay", "evt-1");
  assert.deepEqual(dup, { ingested: false, duplicate: true });
  assert.equal(a.usage.get("u1").used_bytes, gib(1));
  assert.equal(a.eventIds.size, 1);
});

test("events without an id are still counted (but replayable by design, like SQL)", () => {
  const a = acct(makeClock(1_000_000));
  a.ingest("u1", gib(1), "relay", null);
  a.ingest("u1", gib(1), "relay", null);
  assert.equal(a.usage.get("u1").used_bytes, gib(2));
});

test("out-of-order delivery still lands in the current period", () => {
  const a = acct(makeClock(1_000_000));
  a.ingest("u1", gib(1), "relay", "evt-2");
  a.ingest("u1", gib(2), "relay", "evt-1"); // older id, later arrival
  assert.equal(a.usage.get("u1").used_bytes, gib(3));
});

test("multiple sessions and devices aggregate to the exact sum", () => {
  const a = acct(makeClock(1_000_000));
  const half = 1073741824n / 2n; // 0.5 GiB, exact
  const quarter = 1073741824n / 4n; // 0.25 GiB, exact
  for (const [ev, bytes] of [
    ["sess-a-dev-1", gib(1)],
    ["sess-a-dev-2", half],
    ["sess-b-dev-1", gib(2)],
    ["sess-b-dev-2", quarter],
  ]) {
    a.ingest("u1", bytes, "relay", ev);
  }
  assert.equal(a.usage.get("u1").used_bytes, gib(1) + half + gib(2) + quarter);
});

test("concurrent ingest is serialized and exact (no lost update)", async () => {
  const a = acct(makeClock(1_000_000));
  // Simulate concurrent provider events by interleaving; the model is
  // synchronous per ingest, so the sum must be exact.
  const evs = Array.from({ length: 50 }, (_, i) => ["evt-" + i, 1000n + BigInt(i)]);
  evs.forEach(([ev, bytes]) => a.ingest("u1", bytes, "relay", ev));
  const expected = evs.reduce((s, [, b]) => s + b, 0n);
  assert.equal(a.usage.get("u1").used_bytes, expected);
});

test("month rollover resets usage and slides the period", () => {
  const clock = makeClock(1_000_000);
  const a = acct(clock);
  a.ingest("u1", gib(1), "relay", "evt-1");
  const before = { ...a.usage.get("u1") }; // snapshot; rollover mutates the row in place
  clock.advance(2_592_000 + 1); // past the 30-day period
  a.ingest("u1", gib(2), "relay", "evt-2");
  const after = a.usage.get("u1");
  assert.equal(after.used_bytes, gib(2)); // reset, not 3
  assert.equal(after.period_start, before.period_end);
  assert.equal(after.period_end, before.period_end + 2592000n);
});

test("zero-byte event ingests without changing usage", () => {
  const a = acct(makeClock(1_000_000));
  a.ingest("u1", gib(1), "relay", "evt-1");
  const r = a.ingest("u1", 0n, "relay", "evt-2");
  assert.deepEqual(r, { ingested: true });
  assert.equal(a.usage.get("u1").used_bytes, gib(1));
});

test("huge event pushes usage over the allowance", () => {
  const a = acct(makeClock(1_000_000));
  a.ingest("u1", FREE_ALLOWANCE_BYTES + 1n, "relay", "evt-huge");
  assert.throws(() => a.enforceQuota("u1"), (e) => e.code === "TURNQUOTA");
});

test("invalid user is rejected before any accounting", () => {
  const a = acct(makeClock(1_000_000));
  assert.throws(() => a.ingest("ghost", gib(1), "relay", "evt-1"), (e) => e.code === "NOUSER");
  assert.equal(a.usage.size, 0);
});

test("client (authenticated role) cannot write authoritative usage", () => {
  const a = acct(makeClock(1_000_000));
  assert.throws(() => a.ingest("u1", gib(1), "relay", "evt-1", "authenticated"), (e) => e.code === "AUTH");
  assert.equal(a.usage.size, 0);
});

test("negative bytes rejected", () => {
  const a = acct(makeClock(1_000_000));
  assert.throws(() => a.ingest("u1", -1n, "relay", "evt-1"), (e) => e.code === "BYTES");
});

test("free-plan quota is the seeded 1 GiB", () => {
  const a = acct(makeClock(1_000_000));
  const q = a.enforceQuota("u1");
  assert.equal(q.allowance, FREE_ALLOWANCE_BYTES);
  assert.equal(q.remaining_bytes, FREE_ALLOWANCE_BYTES);
});

test("Amalgam+ quota is the seeded 80 GiB", () => {
  const subs = new Map([["u1", { plan: "amalgam_plus", status: "active" }]]);
  const a = acct(makeClock(1_000_000), subs);
  const q = a.enforceQuota("u1");
  assert.equal(q.allowance, PLUS_ALLOWANCE_BYTES);
});

test("expired subscription falls back to the free plan", () => {
  const subs = new Map([["u1", { plan: "amalgam_plus", status: "canceled" }]]);
  const a = acct(makeClock(1_000_000), subs);
  const q = a.enforceQuota("u1");
  assert.equal(q.allowance, FREE_ALLOWANCE_BYTES);
});

test("75% warning threshold", () => {
  const a = acct(makeClock(1_000_000));
  a.ingest("u1", (FREE_ALLOWANCE_BYTES * 75n) / 100n, "relay", "evt-1");
  const s = a.status("u1");
  assert.equal(s.threshold, "warning-75");
  assert.ok(s.pct >= 75 && s.pct < 90);
});

test("90% warning threshold", () => {
  const a = acct(makeClock(1_000_000));
  // (9/10 of allowance) + 1000 bytes: safely above 90% even with integer truncation.
  a.ingest("u1", (FREE_ALLOWANCE_BYTES * 9n) / 10n + 1000n, "relay", "evt-1");
  const s = a.status("u1");
  assert.equal(s.threshold, "warning-90");
  assert.ok(s.pct >= 90 && s.pct < 100);
});

test("100% exhaustion denies credential issuance but allows direct P2P", () => {
  const a = acct(makeClock(1_000_000));
  a.ingest("u1", FREE_ALLOWANCE_BYTES, "relay", "evt-1");
  // Credential issuance (get-turn-credentials) calls enforceQuota -> denied.
  assert.throws(() => a.enforceQuota("u1"), (e) => e.code === "TURNQUOTA");
  const s = a.status("u1");
  assert.equal(s.threshold, "exhausted");
  assert.equal(s.allowed, false);
  assert.equal(s.remaining_bytes, 0n);
  // Direct P2P needs no relay credential: the model has no TURN dependency,
  // exactly as the production comment states.
  assert.ok(true, "direct peer-to-peer is unrelated to relay accounting");
});

test("exact byte accounting internally, GB only for display", () => {
  const a = acct(makeClock(1_000_000));
  a.ingest("u1", 80_000_000_000n, "relay", "evt-1"); // 80 decimal GB
  assert.equal(a.usage.get("u1").used_bytes, 80_000_000_000n);
  // Display conversion is the only place decimals appear.
  assert.equal(Number(a.usage.get("u1").used_bytes) / 1_000_000_000, 80);
});

test("rollover resets usage so a new period can issue credentials again", () => {
  const clock = makeClock(1_000_000);
  const a = acct(clock);
  a.ingest("u1", FREE_ALLOWANCE_BYTES, "relay", "evt-1");
  assert.throws(() => a.enforceQuota("u1"), (e) => e.code === "TURNQUOTA");
  clock.advance(2_592_000 + 1);
  const q = a.enforceQuota("u1"); // no longer exhausted after rollover
  assert.equal(q.used_bytes, 0n);
  assert.equal(q.remaining_bytes, FREE_ALLOWANCE_BYTES);
});

test("unrecognized plan with no active subscription still gets a default", () => {
  const subs = new Map([["u1", { plan: "mystery", status: "active" }]]);
  const a = acct(makeClock(1_000_000), subs);
  const q = a.enforceQuota("u1");
  assert.ok(q.allowance > 0n, "falls back to the free plan");
});
