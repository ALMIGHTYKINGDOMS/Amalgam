// Node registration and heartbeat loop.

import os from "node:os";
import fs from "node:fs";
import { retry } from "./retry.js";

let previousCpuSnapshot = null;

function snapshotCpuTimes(cpus) {
  let idle = 0;
  let total = 0;
  for (const cpu of cpus || []) {
    const times = cpu?.times || {};
    const cpuIdle = Number(times.idle) || 0;
    const cpuTotal = Object.values(times).reduce((sum, value) => sum + (Number(value) || 0), 0);
    idle += cpuIdle;
    total += cpuTotal;
  }
  return { idle, total };
}

// Return null until two real samples exist. Reporting zero on systems without
// load averages (notably Windows) makes an unavailable measurement look like
// an idle host, which is worse than explicitly reporting it as unknown.
export function calculateCpuUsagePct(previous, current) {
  if (!previous || !current) return null;
  const deltaTotal = current.total - previous.total;
  const deltaIdle = current.idle - previous.idle;
  if (!Number.isFinite(deltaTotal) || !Number.isFinite(deltaIdle) || deltaTotal <= 0) return null;
  return Math.round(Math.max(0, Math.min(100, (1 - deltaIdle / deltaTotal) * 100)) * 10) / 10;
}

function sampleCpuUsagePct(cpus) {
  const current = snapshotCpuTimes(cpus);
  const usage = calculateCpuUsagePct(previousCpuSnapshot, current);
  previousCpuSnapshot = current;
  return usage;
}

/**
 * Collect system metrics for the host machine.
 */
export function collectSystemMetrics(storagePath = process.cwd()) {
  const cpus = os.cpus();
  const totalMem = os.totalmem();
  const freeMem = os.freemem();
  const usedMem = totalMem - freeMem;

  let cpuModel = "";
  let cpuCores = cpus.length;
  if (cpus.length > 0) {
    cpuModel = `${cpus[0].model} x${cpus.length}`;
  }

  // Storage applies to the runtime data volume, not an assumed POSIX root.
  // Null represents unavailable data; it must not be converted to a fake zero.
  let storageTotalGb = null;
  let storageUsedGb = null;
  try {
    if (typeof fs.statfsSync === "function") {
      const stat = fs.statfsSync(storagePath || process.cwd());
      storageTotalGb = Math.round((stat.blocks * stat.bsize) / (1024 * 1024 * 1024));
      storageUsedGb = Math.round(((stat.blocks - stat.bfree) * stat.bsize) / (1024 * 1024 * 1024));
    }
  } catch {
    // Best-effort: storage metrics unavailable
  }

  const cpuUsagePct = sampleCpuUsagePct(cpus);

  return {
    cpuModel,
    cpuCores,
    memoryTotalMb: Math.round(totalMem / (1024 * 1024)),
    memoryUsedMb: Math.round(usedMem / (1024 * 1024)),
    storageTotalGb,
    storageUsedGb,
    cpuUsagePct,
  };
}

/**
 * Register (or re-register) the node with the Supabase control plane.
 * Returns { id, reregistered, status }.
 */
export async function registerNode(supabase, config) {
  const metrics = collectSystemMetrics(config.dataDir);

  const { data, error } = await supabase.rpc("register_hosting_node", {
    p_name: config.nodeName,
    p_region: config.region,
    p_host: config.host || os.hostname(),
    p_node_secret: config.nodeSecret,
    p_cpu_model: metrics.cpuModel,
    p_cpu_cores: metrics.cpuCores,
    p_memory_total_mb: metrics.memoryTotalMb,
    p_storage_total_gb: metrics.storageTotalGb,
    p_agent_version: "0.2.0",
    p_tags: [],
  });

  if (error) {
    console.error("[heartbeat] Registration failed:", error.message);
    return { id: null };
  }

  return data;
}

/**
 * Send a heartbeat with current system metrics.
 */
export async function sendHeartbeat(supabase, nodeId, nodeSecret, serverCount,
                                    storagePath = process.cwd(), sampledMetrics = null) {
  const metrics = sampledMetrics || collectSystemMetrics(storagePath);

  try {
    await retry(async () => {
      const { error } = await supabase.rpc("node_heartbeat", {
        p_node_id: nodeId,
        p_node_secret: nodeSecret,
        p_status: "online",
        p_cpu_usage_pct: metrics.cpuUsagePct,
        p_memory_used_mb: metrics.memoryUsedMb,
        p_storage_used_gb: metrics.storageUsedGb,
        p_server_count: serverCount,
      });
      if (error) throw new Error(error.message || "heartbeat RPC failed");
    }, { label: "node-heartbeat", maxRetries: 2, baseDelayMs: 1000 });
    return true;
  } catch (err) {
    console.error("[heartbeat] Heartbeat error:", err.message);
    return false;
  }
}

/**
 * Start the heartbeat loop. Calls the control plane every interval.
 * @param {boolean} () => running - function that returns false to stop
 */
export function heartbeatLoop(supabase, nodeInfo, config, isRunning, eventCollector, serverCountProvider = () => 0) {
  const interval = config.heartbeatIntervalSec * 1000;

  const loop = async () => {
    while (isRunning()) {
      try {
        const metrics = collectSystemMetrics(config.dataDir);
        const serverCount = Number(serverCountProvider()) || 0;
        await sendHeartbeat(
          supabase,
          nodeInfo.id,
          config.nodeSecret,
          serverCount,
          config.dataDir,
          metrics
        );

        // Record detailed system metrics via event collector
        if (eventCollector) {
          eventCollector.heartbeatSent(nodeInfo.id, {
            cpuUsagePct: metrics.cpuUsagePct,
            memoryUsedMb: metrics.memoryUsedMb,
            memoryTotalMb: metrics.memoryTotalMb,
            storageUsedGb: metrics.storageUsedGb,
            storageTotalGb: metrics.storageTotalGb,
            cpuCores: metrics.cpuCores,
            serverCount,
          });

          // Record individual system metrics for time-series analysis
          eventCollector.metric("system.cpu_pct", metrics.cpuUsagePct, { unit: "%" });
          eventCollector.metric("system.memory_used_mb", metrics.memoryUsedMb, { unit: "MB" });
          eventCollector.metric("system.memory_total_mb", metrics.memoryTotalMb, { unit: "MB" });
          eventCollector.metric("system.memory_pct",
            metrics.memoryTotalMb > 0 ? Math.round((metrics.memoryUsedMb / metrics.memoryTotalMb) * 1000) / 10 : 0,
            { unit: "%" }
          );
          eventCollector.metric("system.storage_used_gb", metrics.storageUsedGb, { unit: "GB" });
          eventCollector.metric("system.storage_total_gb", metrics.storageTotalGb, { unit: "GB" });
          eventCollector.metric("system.cpu_cores", metrics.cpuCores);
        }
      } catch (err) {
        console.error("[heartbeat] Error:", err.message);
        if (eventCollector) {
          eventCollector.networkError("heartbeat", err.message);
        }
      }
      await sleep(interval);
    }
  };

  loop();
  console.log(`[heartbeat] Loop started (interval: ${config.heartbeatIntervalSec}s)`);
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}
