import test from "node:test";
import assert from "node:assert/strict";
import { parseMinecraftMetrics } from "./console.js";

test("parseMinecraftMetrics extracts server diagnostics", () => {
  const metrics = parseMinecraftMetrics([
    { content: "TPS: 19.8 avg tick time: 5.23ms" },
    { content: "3 of a max 20 players" },
    { content: "Entity count: 1523 Loaded chunks: 4567" },
    { content: "[GC (Allocation Failure) 456M->123M(1024M), 0.123 secs]" },
  ]);
  assert.equal(metrics.tps, 19.8);
  assert.equal(metrics.mspt, 5.23);
  assert.equal(metrics.playerCount, 3);
  assert.equal(metrics.metadata.max_players, 20);
  assert.equal(metrics.entityCount, 1523);
  assert.equal(metrics.chunkCount, 4567);
  assert.equal(metrics.heapUsedMb, 123);
  assert.equal(metrics.heapMaxMb, 1024);
  assert.equal(metrics.gcCount, 1);
});

test("parseMinecraftMetrics preserves missing diagnostics as unknown", () => {
  const metrics = parseMinecraftMetrics([]);
  assert.equal(metrics.tps, null);
  assert.equal(metrics.mspt, null);
  assert.equal(metrics.playerCount, null);
  assert.equal(metrics.entityCount, null);
  assert.equal(metrics.chunkCount, null);
  assert.equal(metrics.threadCount, null);
  assert.equal(metrics.heapUsedMb, null);
  assert.equal(metrics.heapMaxMb, null);
  assert.equal(metrics.gcCount, null);
  assert.equal(metrics.gcTimeMs, null);
});
