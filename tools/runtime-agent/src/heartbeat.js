// Node registration and heartbeat loop.

import os from "node:os";
import fs from "node:fs";
import { retry } from "./retry.js";

/**
 * Collect system metrics for the host machine.
 */
export function collectSystemMetrics() {
  const cpus = os.cpus();
  const totalMem = os.totalmem();
  const freeMem = os.freemem();
  const usedMem = totalMem - freeMem;

  let cpuModel = "";
  let cpuCores = cpus.length;
  if (cpus.length > 0) {
    cpuModel = `${cpus[0].model} x${cpus.length}`;
  }

  // Storage: use Node.js fs.statfsSync (Node 18+)
  let storageTotalGb = 0;
  let storageUsedGb = 0;
  try {
    if (typeof fs.statfsSync === "function") {
      const stat = fs.statfsSync("/");
      storageTotalGb = Math.round((stat.blocks * stat.bsize) / (1024 * 1024 * 1024));
      storageUsedGb = Math.round(((stat.blocks - stat.bfree) * stat.bsize) / (1024 * 1024 * 1024));
    }
  } catch {
    // Best-effort: storage metrics unavailable
  }

  // CPU usage: read from /proc or compute from os.loadavg
  let cpuUsagePct = 0;
  try {
    if (process.platform === "linux") {
      const stat = os.loadavg();
      cpuUsagePct = Math.min(100, (stat[0] / cpuCores) * 100);
    } else {
      // Windows/macOS: use load average approximation
      const stat = os.loadavg();
      cpuUsagePct = Math.min(100, (stat[0] / cpuCores) * 100);
    }
  } catch {
    // Ignore
  }

  return {
    cpuModel,
    cpuCores,
    memoryTotalMb: Math.round(totalMem / (1024 * 1024)),
    memoryUsedMb: Math.round(usedMem / (1024 * 1024)),
    storageTotalGb,
    storageUsedGb,
    cpuUsagePct: Math.round(cpuUsagePct * 10) / 10,
  };
}

/**
 * Register (or re-register) the node with the Supabase control plane.
 * Returns { id, reregistered, status }.
 */
export async function registerNode(supabase, config) {
  const metrics = collectSystemMetrics();

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
export async function sendHeartbeat(supabase, nodeId, nodeSecret, serverCount) {
  const metrics = collectSystemMetrics();

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
        const metrics = collectSystemMetrics();
        const serverCount = Number(serverCountProvider()) || 0;
        await sendHeartbeat(
          supabase,
          nodeInfo.id,
          config.nodeSecret,
          serverCount
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
