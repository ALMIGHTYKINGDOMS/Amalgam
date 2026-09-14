// Operation polling and execution.
// Polls Supabase for pending operations, claims them, and dispatches
// to the appropriate handler (start, stop, restart, command, backup, etc.).

import { retry } from './retry.js';
import fs from "node:fs";
import crypto from "node:crypto";
import { Readable } from "node:stream";
import { finished } from "node:stream/promises";

/**
 * Fetch pending operations for this node and execute them.
 */
let eventCollector = null;

export function setEventCollector(collector) {
  eventCollector = collector;
}

export async function operationPollLoop(supabase, nodeInfo, config, isRunning, manager) {
  const interval = config.operationPollIntervalSec * 1000;

  console.log(
    `[ops] Operation poll loop started (interval: ${config.operationPollIntervalSec}s)`
  );

  while (isRunning()) {
    try {
      // Check for timed-out operations
      await checkTimeouts(supabase, nodeInfo, config);

      // Fetch pending operations with retry
      let pendingOps = null;
      let fetchError = null;
      try {
        const result = await retry(
          () => supabase
            .from("server_operations")
            .select("*")
            .eq("status", "pending")
            .order("created_at", { ascending: true })
            .limit(config.maxConcurrentOps),
          { label: "fetch-operations", maxRetries: 2, baseDelayMs: 2000 }
        );
        pendingOps = result.data;
        fetchError = result.error;
      } catch (err) {
        fetchError = { message: err.message };
      }

      if (fetchError) {
        console.error("[ops] Fetch error:", fetchError.message);
        await sleep(interval);
        continue;
      }

      if (!pendingOps || pendingOps.length === 0) {
        await sleep(interval);
        continue;
      }

      console.log(`[ops] Found ${pendingOps.length} pending operation(s)`);

      // Process each operation
      for (const op of pendingOps) {
        await processOperation(supabase, nodeInfo, config, manager, op);
      }
    } catch (err) {
      console.error("[ops] Error in poll loop:", err.message);
    }

    await sleep(interval);
  }
}

/**
 * Process a single operation: claim it, execute it, and report the result.
 */
async function processOperation(supabase, nodeInfo, config, manager, op) {
  const opId = op.id;
  const serverId = op.server_id;
  const operation = op.operation;

  console.log(`[ops] Processing operation ${opId}: ${operation}`);

  const opStartTime = Date.now();

  if (eventCollector) {
    eventCollector.operationStarted(opId, operation, serverId);
  }

  // Claim the operation
  const { data: claimed, error: claimError } = await supabase.rpc(
    "claim_operation",
    {
      p_operation_id: opId,
      p_node_id: nodeInfo.id,
      p_node_secret: config.nodeSecret,
    }
  );

  if (claimError) {
    console.error(`[ops] Failed to claim operation ${opId}:`, claimError.message);
    return;
  }

  // Start tracking the operation and fail closed if the claim changed.
  const { error: startError } = await supabase.rpc("start_operation", {
    p_operation_id: opId,
    p_node_id: nodeInfo.id,
    p_node_secret: config.nodeSecret,
  });
  if (startError) {
    console.error(`[ops] Failed to start operation ${opId}:`, startError.message);
    return;
  }

  // Fetch the server instance details
  const { data: instance, error: instanceError } = await supabase
    .from("server_instances")
    .select("*")
    .eq("id", serverId)
    .single();

  if (instanceError || !instance) {
    await completeOp(supabase, nodeInfo, config, opId, false, {
      error: `Server instance not found: ${instanceError?.message || "unknown"}`,
    }, operation, opStartTime);
    return;
  }

  // Dispatch to the appropriate handler
  let result;
  try {
    switch (operation) {
      case "start":
        result = await handleStart(supabase, nodeInfo, config, manager, instance, op);
        break;
      case "stop":
        result = await handleStop(manager, instance);
        break;
      case "restart":
        result = await handleRestart(supabase, nodeInfo, config, manager, instance, op);
        break;
      case "command":
        result = await handleCommand(manager, instance, op);
        break;
      case "backup":
        result = await handleBackup(supabase, nodeInfo, config, manager, instance, op);
        break;
      case "restore":
        result = await handleRestore(manager, instance, op);
        break;
      case "settings":
        result = await handleSettings(manager, instance, op);
        break;
      case "install_jar":
        result = await handleInstallJar(manager, instance, op);
        break;
      case "delete":
        result = await handleDelete(supabase, nodeInfo, config, manager, instance, op);
        break;
      default:
        result = { success: false, error: `Unknown operation: ${operation}` };
    }
  } catch (err) {
    result = { success: false, error: err.message };
  }

  await completeOp(supabase, nodeInfo, config, opId, result.success, result, operation, opStartTime);
}

// ---------------------------------------------------------------------------
// Operation Handlers
// ---------------------------------------------------------------------------

async function handleStart(supabase, nodeInfo, config, manager, instance, op) {
  // Apply settings from the operation params if provided
  if (op.params && Object.keys(op.params).length > 0) {
    manager.applySettings(instance, op.params);
  }

  const startResult = manager.start(instance);
  if (!startResult.success) {
    return startResult;
  }

  // Update instance status in Supabase
  await supabase.rpc("update_server_instance", {
    p_server_id: instance.id,
    p_node_id: nodeInfo.id,
    p_node_secret: config.nodeSecret,
    p_status: "running",
    p_pid: startResult.pid,
  });

  return { success: true, pid: startResult.pid };
}

async function handleStop(manager, instance) {
  const stopResult = manager.stop(instance.id);
  return stopResult;
}

async function handleRestart(supabase, nodeInfo, config, manager, instance, op) {
  // Stop first
  if (manager.isRunning(instance.id)) {
    const stopResult = manager.stop(instance.id);
    if (!stopResult.success) {
      return stopResult;
    }
    // Wait for process to exit
    await sleep(3000);
  }

  // Start
  return handleStart(supabase, nodeInfo, config, manager, instance, op);
}

async function handleCommand(manager, instance, op) {
  const command = op.command || op.params?.command || "";
  if (!command) {
    return { success: false, error: "No command provided" };
  }
  return manager.command(instance.id, command);
}

async function handleBackup(supabase, nodeInfo, config, manager, instance, op) {
  const label = op.params?.label || "";
  const backupResult = manager.backup(instance, label);

  if (backupResult.success) {
    // Record the backup in the server instance
    await supabase.rpc("update_server_instance", {
      p_server_id: instance.id,
      p_node_id: nodeInfo.id,
      p_node_secret: config.nodeSecret,
      p_disk_usage_mb: Math.round(backupResult.size_bytes / (1024 * 1024)),
    });
  }

  return backupResult;
}

async function handleRestore(manager, instance, op) {
  const backupPath = op.params?.backup_path || "";
  if (!backupPath) {
    return { success: false, error: "No backup path provided" };
  }
  return manager.restore(instance, backupPath);
}

async function handleSettings(manager, instance, op) {
  const settings = op.params || {};
  return manager.applySettings(instance, settings);
}

async function handleInstallJar(manager, instance, op) {
  const jarUrl = String(op.params?.url || "").trim();
  const serverDir = manager.getServerDir(instance.id, instance.name);
  const jarPath = `${serverDir}/server.jar`;
  const partPath = `${jarPath}.part`;

  if (!jarUrl) return { success: false, error: "No jar URL provided" };

  let parsedUrl;
  try {
    parsedUrl = new URL(jarUrl);
  } catch {
    return { success: false, error: "Invalid jar URL" };
  }
  if (parsedUrl.protocol !== "https:") {
    return { success: false, error: "Server jar downloads must use HTTPS" };
  }

  try {
    const response = await fetch(parsedUrl, { redirect: "error" });
    if (!response.ok || !response.body) {
      return { success: false, error: `Jar download failed: HTTP ${response.status}` };
    }
    const declaredLength = Number(response.headers.get("content-length") || 0);
    if (declaredLength > 2 * 1024 * 1024 * 1024) {
      return { success: false, error: "Server jar exceeds the 2 GiB limit" };
    }

    fs.mkdirSync(serverDir, { recursive: true });
    const output = fs.createWriteStream(partPath, { flags: "w" });
    Readable.fromWeb(response.body).pipe(output);
    await finished(output);

    const expectedHash = String(op.params?.sha256 || "").toLowerCase();
    if (expectedHash) {
      if (!/^[a-f0-9]{64}$/.test(expectedHash)) {
        fs.rmSync(partPath, { force: true });
        return { success: false, error: "sha256 must be a 64-character hexadecimal digest" };
      }
      const hash = crypto.createHash("sha256");
      for await (const chunk of fs.createReadStream(partPath)) hash.update(chunk);
      if (hash.digest("hex") !== expectedHash) {
        fs.rmSync(partPath, { force: true });
        return { success: false, error: "Server jar SHA-256 verification failed" };
      }
    }

    fs.renameSync(partPath, jarPath);
    return { success: true, path: jarPath, sha256: expectedHash || undefined };
  } catch (err) {
    fs.rmSync(partPath, { force: true });
    return { success: false, error: err.message };
  }
}

async function handleDelete(supabase, nodeInfo, config, manager, instance, op) {
  // Stop if running
  if (manager.isRunning(instance.id)) {
    manager.stop(instance.id);
    await sleep(2000);
  }

  // Delete the server directory
  const serverDir = manager.getServerDir(instance.id, instance.name);
  try {
    const fs = await import("node:fs");
    if (fs.existsSync(serverDir)) {
      fs.rmSync(serverDir, { recursive: true, force: true });
    }
  } catch {
    // Best-effort
  }

  // Delete the instance record from Supabase
  const { error } = await supabase
    .from("server_instances")
    .delete()
    .eq("id", instance.id)
    .eq("node_id", nodeInfo.id);

  if (error) {
    return { success: false, error: error.message };
  }

  return { success: true };
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

async function completeOp(supabase, nodeInfo, config, opId, success, result, operationType, startTime) {
  const errorMsg = success ? "" : result.error || "Unknown error";
  const durationMs = startTime ? Date.now() - startTime : 0;

  // The params handling: convert result to a JSON-safe format
  const resultJson = success
    ? typeof result === "object"
      ? result
      : { message: String(result) }
    : {};

  try {
    await supabase.rpc("complete_operation", {
      p_operation_id: opId,
      p_node_id: nodeInfo.id,
      p_node_secret: config.nodeSecret,
      p_success: success,
      p_result: resultJson,
      p_error_message: errorMsg,
    });
    console.log(
      `[ops] Operation ${opId} ${success ? "completed" : "failed"}: ${errorMsg || "OK"} (${durationMs}ms)`
    );

    if (eventCollector) {
      eventCollector.operationCompleted(opId, operationType || "unknown", success, durationMs);
      if (!success && errorMsg) {
        eventCollector.error("operation", errorMsg, {
          component: "OperationHandler",
          operation: operationType || "unknown",
          properties: { operation_id: opId },
        });
      }
    }
  } catch (err) {
    console.error(`[ops] Failed to complete operation ${opId}:`, err.message);
    if (eventCollector) {
      eventCollector.networkError(`complete_operation:${opId}`, err.message);
    }
  }
}

/**
 * Check for operations that have been running too long and mark them as failed.
 */
export async function checkTimeouts(supabase, nodeInfo, config) {
  try {
    const { data: stuckOps } = await supabase
      .from("server_operations")
      .select("id, operation, server_id, claimed_at, started_at, timeout_seconds")
      .in("status", ["claimed", "running"])
      .eq("node_id", nodeInfo.id);

    if (!stuckOps || stuckOps.length === 0) return;

    const now = Date.now();
    for (const op of stuckOps) {
      const startTime = op.started_at ? new Date(op.started_at).getTime()
        : op.claimed_at ? new Date(op.claimed_at).getTime() : 0;
      const timeout = (op.timeout_seconds || 300) * 1000;

      if (startTime > 0 && (now - startTime) > timeout) {
        console.warn(`[ops] Operation ${op.id} timed out after ${timeout / 1000}s`);
        await supabase.rpc("complete_operation", {
          p_operation_id: op.id,
          p_node_id: nodeInfo.id,
          p_node_secret: config.nodeSecret,
          p_success: false,
          p_result: { timed_out: true },
          p_error_message: `Operation timed out after ${timeout / 1000}s`,
        });

        if (eventCollector) {
          eventCollector.error("operation", `Operation timed out: ${op.operation}`, {
            component: "OperationTimeout",
            operation: op.operation,
            properties: { operation_id: op.id, server_id: op.server_id },
          });
        }
      }
    }
  } catch (err) {
    console.error("[ops] Error checking timeouts:", err.message);
  }
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}
