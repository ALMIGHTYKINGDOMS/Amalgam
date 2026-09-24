// Operation polling and execution.
// Polls Supabase for pending operations, claims them, and dispatches
// to the appropriate handler (start, stop, restart, command, backup, etc.).

import { retry } from './retry.js';
import fs from "node:fs";
import crypto from "node:crypto";
import path from "node:path";
import { isIP } from "node:net";
import { Readable, Transform } from "node:stream";
import { pipeline } from "node:stream/promises";

/**
 * Fetch pending operations for this node and execute them.
 */
let eventCollector = null;
const MAX_SERVER_JAR_BYTES = 2 * 1024 * 1024 * 1024;

export function setEventCollector(collector) {
  eventCollector = collector;
}

function sameIdentifier(left, right) {
  return typeof left === "string"
    && typeof right === "string"
    && left.toLowerCase() === right.toLowerCase();
}

function isOperationRoutedToNode(op, nodeId) {
  return Boolean(
    op
    && typeof op.id === "string"
    && typeof op.server_id === "string"
    && sameIdentifier(op.node_id, nodeId)
  );
}

function isClaimForOperation(claimed, op, nodeId) {
  return Boolean(
    claimed
    && claimed.status === "claimed"
    && sameIdentifier(claimed.id, op.id)
    && sameIdentifier(claimed.server_id, op.server_id)
    && sameIdentifier(claimed.node_id, nodeId)
  );
}

function isInstanceAssignedToNode(instance, serverId, nodeId) {
  return Boolean(
    instance
    && sameIdentifier(instance.id, serverId)
    && sameIdentifier(instance.node_id, nodeId)
  );
}

/**
 * Read only the pending queue assigned to this node. The database claim RPC
 * repeats this predicate atomically; this filter limits visibility and avoids
 * asking an unrelated agent to contend for another node's work.
 */
export async function fetchPendingOperations(supabase, nodeInfo, maxConcurrentOps) {
  if (!nodeInfo?.id) {
    throw new Error("Cannot poll operations without a runtime node ID");
  }

  return retry(
    () => supabase
      .from("server_operations")
      .select("*")
      .eq("status", "pending")
      .eq("node_id", nodeInfo.id)
      .order("created_at", { ascending: true })
      .limit(maxConcurrentOps),
    { label: "fetch-operations", maxRetries: 2, baseDelayMs: 2000 }
  );
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
        const result = await fetchPendingOperations(
          supabase,
          nodeInfo,
          config.maxConcurrentOps
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
export async function processOperation(supabase, nodeInfo, config, manager, op) {
  const opId = op.id;
  const serverId = op.server_id;
  const operation = op.operation;

  // A stale poll result must not be enough to execute work. The claim RPC
  // below enforces the same relationship atomically, but this local check
  // avoids sending malformed or cross-node work to the control plane at all.
  if (!isOperationRoutedToNode(op, nodeInfo?.id)) {
    console.warn(
      `[ops] Ignoring operation ${opId || "unknown"}: it is not routed to node ${nodeInfo?.id || "unknown"}`
    );
    return;
  }

  console.log(`[ops] Processing operation ${opId}: ${operation}`);

  const opStartTime = Date.now();

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

  const claimedOperation = Array.isArray(claimed) ? claimed[0] : claimed;
  if (!isClaimForOperation(claimedOperation, op, nodeInfo.id)) {
    console.error(
      `[ops] Ignoring operation ${opId}: claim response does not confirm this node and server assignment`
    );
    return;
  }

  // Re-read the assigned instance immediately before execution. The query is
  // scoped server-side and the explicit check protects the dispatch boundary
  // even if a caller supplies a stale or malformed client result.
  const { data: instance, error: instanceError } = await supabase
    .from("server_instances")
    .select("*")
    .eq("id", serverId)
    .eq("node_id", nodeInfo.id)
    .single();

  if (instanceError || !isInstanceAssignedToNode(instance, serverId, nodeInfo.id)) {
    await completeOp(supabase, nodeInfo, config, opId, false, {
      error: instanceError
        ? `Server instance cannot be verified for this node: ${instanceError.message || "unknown"}`
        : "Server instance is no longer assigned to this node",
    }, operation, opStartTime);
    return;
  }

  // start_operation repeats the operation/server/node check atomically. It
  // must succeed before a local process, filesystem, or command handler runs.
  const { data: startResult, error: startError } = await supabase.rpc("start_operation", {
    p_operation_id: opId,
    p_node_id: nodeInfo.id,
    p_node_secret: config.nodeSecret,
  });
  if (startError || startResult?.success !== true) {
    console.error(
      `[ops] Failed to start operation ${opId}:`,
      startError?.message || "start RPC did not confirm the node assignment"
    );
    return;
  }

  if (eventCollector) {
    eventCollector.operationStarted(opId, operation, serverId);
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

export async function handleRestart(supabase, nodeInfo, config, manager, instance, op) {
  // Stop first
  if (manager.isRunning(instance.id)) {
    const stopResult = manager.stop(instance.id);
    if (!stopResult.success) {
      return stopResult;
    }
    // Do not start a second server process until the first has actually
    // exited. A fixed delay is neither a file-lock guarantee nor proof that
    // the old process stopped accepting commands.
    const exited = await manager.waitForExit(instance.id);
    if (!exited || manager.isRunning(instance.id)) {
      return { success: false, error: "Server did not exit before restart" };
    }
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

export async function handleRestore(manager, instance, op) {
  if (manager.isRunning(instance.id)) {
    return { success: false, error: "Stop the server before restoring a backup" };
  }
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

export async function handleInstallJar(manager, instance, op) {
  const jarUrl = String(op.params?.url || "").trim();
  const expectedHash = String(op.params?.sha256 || "").trim().toLowerCase();
  const serverDir = manager.getServerDir(instance.id, instance.name);
  const jarPath = path.join(serverDir, "server.jar");
  const partPath = `${jarPath}.part`;

  if (!jarUrl) return { success: false, error: "No jar URL provided" };
  if (!/^[a-f0-9]{64}$/.test(expectedHash)) {
    return { success: false, error: "A 64-character SHA-256 digest is required for every server jar" };
  }

  let parsedUrl;
  try {
    parsedUrl = new URL(jarUrl);
  } catch {
    return { success: false, error: "Invalid jar URL" };
  }
  if (parsedUrl.protocol !== "https:") {
    return { success: false, error: "Server jar downloads must use HTTPS" };
  }
  if (parsedUrl.username || parsedUrl.password || isUnsafeArtifactHost(parsedUrl.hostname)) {
    return { success: false, error: "Server jar URL must use a public HTTPS host without credentials" };
  }

  try {
    const response = await fetch(parsedUrl, { redirect: "error" });
    if (!response.ok || !response.body) {
      return { success: false, error: `Jar download failed: HTTP ${response.status}` };
    }
    const contentLength = response.headers.get("content-length");
    const declaredLength = contentLength === null ? null : Number(contentLength);
    if (declaredLength !== null &&
        (!Number.isSafeInteger(declaredLength) || declaredLength < 0)) {
      return { success: false, error: "Server jar has an invalid Content-Length" };
    }
    if (declaredLength !== null && declaredLength > MAX_SERVER_JAR_BYTES) {
      return { success: false, error: "Server jar exceeds the 2 GiB limit" };
    }

    fs.mkdirSync(serverDir, { recursive: true });
    fs.rmSync(partPath, { force: true });
    const hash = crypto.createHash("sha256");
    let receivedBytes = 0;
    const integrityGate = new Transform({
      transform(chunk, encoding, callback) {
        const bytes = Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk, encoding);
        if (receivedBytes + bytes.length > MAX_SERVER_JAR_BYTES) {
          callback(new Error("Server jar exceeds the 2 GiB limit"));
          return;
        }
        receivedBytes += bytes.length;
        hash.update(bytes);
        callback(null, bytes);
      },
    });
    await pipeline(
      Readable.fromWeb(response.body),
      integrityGate,
      fs.createWriteStream(partPath, { flags: "wx" })
    );

    if (receivedBytes === 0) {
      fs.rmSync(partPath, { force: true });
      return { success: false, error: "Server jar download was empty" };
    }
    if (declaredLength !== null && receivedBytes !== declaredLength) {
      fs.rmSync(partPath, { force: true });
      return { success: false, error: "Server jar Content-Length did not match the downloaded bytes" };
    }
    if (hash.digest("hex") !== expectedHash) {
      fs.rmSync(partPath, { force: true });
      return { success: false, error: "Server jar SHA-256 verification failed" };
    }

    fs.renameSync(partPath, jarPath);
    return { success: true, path: jarPath, sha256: expectedHash, size_bytes: receivedBytes };
  } catch (err) {
    fs.rmSync(partPath, { force: true });
    return {
      success: false,
      error: err instanceof Error ? err.message : "Server jar download failed",
    };
  }
}

export async function handleDelete(supabase, nodeInfo, config, manager, instance, op) {
  // Stop an active process first, then wait for its exit event. A ChildProcess
  // can have a kill signal pending while it still owns files, so a timer (or
  // the `killed` flag alone) is not proof that it is safe to remove storage.
  if (manager.isRunning(instance.id)) {
    const stopResult = manager.stop(instance.id);
    if (!stopResult?.success && manager.isRunning(instance.id)) {
      return { success: false, error: stopResult?.error || "Unable to stop server before deletion" };
    }
  }

  let exited = false;
  try {
    exited = await manager.waitForExit(instance.id);
  } catch (err) {
    const message = err instanceof Error ? err.message : String(err);
    return { success: false, error: `Unable to confirm server exit: ${message}` };
  }
  if (!exited || manager.isRunning(instance.id)) {
    return { success: false, error: "Server process did not exit; refusing to delete server data" };
  }

  // Move data into the node's recovery area before removing the control-plane
  // record. A filesystem delete cannot be rolled back if the database call
  // fails or an operator discovers the deletion was premature.
  const serverDir = manager.getServerDir(instance.id, instance.name);
  let recoveryPath = null;
  try {
    if (fs.existsSync(serverDir)) {
      recoveryPath = createRecoveryPath(manager, instance.id);
      fs.renameSync(serverDir, recoveryPath);
    }
  } catch (err) {
    return {
      success: false,
      error: `Unable to move server data to recovery before deletion: ${err instanceof Error ? err.message : String(err)}`,
    };
  }

  // Delete the instance record from Supabase
  const { error } = await supabase
    .from("server_instances")
    .delete()
    .eq("id", instance.id)
    .eq("node_id", nodeInfo.id);

  if (error) {
    if (recoveryPath && fs.existsSync(recoveryPath) && !fs.existsSync(serverDir)) {
      try {
        fs.renameSync(recoveryPath, serverDir);
        recoveryPath = null;
      } catch (restoreError) {
        return {
          success: false,
          error: `${error.message}; server data is retained at ${recoveryPath} because rollback failed: ${restoreError instanceof Error ? restoreError.message : String(restoreError)}`,
          recovery_path: recoveryPath,
        };
      }
    }
    return { success: false, error: error.message };
  }

  return recoveryPath ? { success: true, recovery_path: recoveryPath } : { success: true };
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

function isUnsafeArtifactHost(hostname) {
  const host = String(hostname || "").toLowerCase();
  if (!host || host === "localhost" || host.endsWith(".localhost") || host.endsWith(".local")) {
    return true;
  }
  if (isIP(host) === 6) {
    return host === "::1" || host.startsWith("fc") || host.startsWith("fd") || host.startsWith("fe80:");
  }
  if (isIP(host) !== 4) return false;
  const [a, b] = host.split(".").map((part) => Number(part));
  return a === 10 || a === 127 || a === 0 ||
    (a === 169 && b === 254) ||
    (a === 172 && b >= 16 && b <= 31) ||
    (a === 192 && b === 168);
}

function createRecoveryPath(manager, serverId) {
  if (!manager?.dataDir) throw new Error("Runtime data directory is unavailable");
  const dataRoot = path.resolve(manager.dataDir);
  const recoveryRoot = path.resolve(dataRoot, ".amalgam-trash", "servers");
  const relative = path.relative(dataRoot, recoveryRoot);
  if (!relative || relative === ".." || relative.startsWith(`..${path.sep}`) || path.isAbsolute(relative)) {
    throw new Error("Recovery directory is outside the runtime data directory");
  }
  fs.mkdirSync(recoveryRoot, { recursive: true });
  return path.join(recoveryRoot, `server-${serverId}-${Date.now()}-${crypto.randomUUID()}`);
}

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
