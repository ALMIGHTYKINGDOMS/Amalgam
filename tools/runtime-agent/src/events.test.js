import test from "node:test";
import assert from "node:assert/strict";
import { EventCollector } from "./events.js";

test("EventCollector flushes each populated batch once", async () => {
  const calls = [];
  const supabase = { rpc: async (name, args) => { calls.push([name, args]); return { error: null }; } };
  const collector = new EventCollector(supabase, { heartbeatIntervalSec: 30 }, "node-1");
  collector.event("server", "started", { id: "s1" });
  collector.metric("cpu", 42, { unit: "%" });
  await collector.flush();
  assert.deepEqual(calls.map(([name]) => name).sort(), ["insert_events", "insert_metrics"]);
  assert.equal(collector.eventBuffer.length, 0);
  assert.equal(collector.metricBuffer.length, 0);
});

test("EventCollector requeues a batch after a terminal failure", async () => {
  const supabase = { rpc: async () => ({ error: new Error("offline") }) };
  const collector = new EventCollector(supabase, { heartbeatIntervalSec: 30 }, "node-1");
  collector._flushWithRetry = async () => false;
  collector.event("network", "offline");
  await collector.flush();
  assert.equal(collector.eventBuffer.length, 1);
});

test("EventCollector does not turn unavailable metrics into zero", () => {
  const collector = new EventCollector({}, { heartbeatIntervalSec: 30 }, "node-1");
  assert.equal(collector.metric("missing", null), false);
  assert.equal(collector.metric("missing", undefined), false);
  assert.equal(collector.metric("missing", ""), false);
  assert.equal(collector.metric("missing", false), false);
  assert.equal(collector.metricBuffer.length, 0);

  assert.equal(collector.metric("observed-zero", 0), true);
  assert.deepEqual(collector.metricBuffer.map((metric) => metric.value), [0]);
});
