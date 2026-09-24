import test from "node:test";
import assert from "node:assert/strict";
import path from "node:path";
import os from "node:os";
import crypto from "node:crypto";
import { calculateCpuUsagePct, collectSystemMetrics } from "./heartbeat.js";

test("calculateCpuUsagePct uses CPU time deltas and keeps an unsampled value unknown", () => {
  assert.equal(calculateCpuUsagePct(null, { idle: 160, total: 1200 }), null);
  assert.equal(calculateCpuUsagePct({ idle: 100, total: 1000 }, { idle: 100, total: 1000 }), null);
  assert.equal(calculateCpuUsagePct({ idle: 100, total: 1000 }, { idle: 160, total: 1200 }), 70);
});

test("collectSystemMetrics treats an unavailable requested data volume as unknown", () => {
  const missingDataDirectory = path.join(os.tmpdir(), `amalgam-no-volume-${crypto.randomUUID()}`);
  const metrics = collectSystemMetrics(missingDataDirectory);
  assert.equal(metrics.storageTotalGb, null);
  assert.equal(metrics.storageUsedGb, null);
});
