// Minecraft server process management.
// Spawns Java processes, captures stdout/stderr, manages lifecycle.

import { spawn } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { appendConsoleLine, recoverConsoleLog } from "./console-log.js";

// server_instances.id is a PostgreSQL UUID. Keep the storage key tied to that
// immutable control-plane identity, rather than to a mutable, user-provided
// display name. The explicit validation also makes this boundary fail closed
// if a malformed operation reaches the agent.
const SERVER_ID_PATTERN = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;

// Keep server.properties as a typed, one-line key/value document.  Accepting
// arbitrary keys or newlines here would let an operation payload add unrelated
// settings (for example, an injected online-mode=false line).  The allow-list
// covers the standard Java server properties that Amalgam deliberately exposes
// while keeping the filesystem and process boundary closed to surprise keys.
const SERVER_PROPERTY_DEFAULTS = Object.freeze({
  "server-port": 25565,
  "max-players": 20,
  "motd": "An Amalgam Server",
  "gamemode": "survival",
  "difficulty": "normal",
  "whitelist": false,
  "online-mode": true,
  "level-name": "world",
  "level-seed": "",
  "view-distance": 10,
  "simulation-distance": 10,
  "max-world-size": 29999984,
  "pvp": true,
  "allow-nether": true,
  "spawn-protection": 16,
  "enable-command-block": false,
  "spawn-monsters": true,
  "spawn-animals": true,
  "spawn-npcs": true,
  "generate-structures": true,
  "allow-flight": false,
  "force-gamemode": false,
  "hardcore": false,
  "white-list": false,
  "enforce-whitelist": false,
  "rate-limit": 0,
  "prevent-proxy-connections": false,
  "use-native-transport": true,
  "entity-broadcast-range-percentage": 100,
});

const ALLOWED_SERVER_PROPERTIES = new Set([
  ...Object.keys(SERVER_PROPERTY_DEFAULTS),
  "accepts-transfers", "broadcast-console-to-ops", "broadcast-rcon-to-ops",
  "bug-report-link", "debug", "enable-jmx-monitoring", "enable-query", "enable-rcon",
  "enable-status", "enforce-secure-profile", "function-permission-level", "generator-settings",
  "hide-online-players", "initial-disabled-packs", "initial-enabled-packs", "level-type",
  "log-ips", "max-chained-neighbor-updates", "max-tick-time", "network-compression-threshold",
  "op-permission-level", "pause-when-empty-seconds", "query.port", "rcon.password", "rcon.port",
  "region-file-compression", "require-resource-pack", "resource-pack", "resource-pack-id",
  "resource-pack-prompt", "resource-pack-sha1", "server-ip", "sync-chunk-writes",
  "text-filtering-config",
]);

function serverStorageKey(serverId) {
  if (typeof serverId !== "string" || !SERVER_ID_PATTERN.test(serverId)) {
    throw new Error("Server ID must be a UUID before it can be used for storage");
  }

  // UUIDs are case-insensitive, while filesystem case behavior differs by
  // platform. Canonicalizing prevents case-only aliases from creating a
  // second directory for the same database identity.
  return `server-${serverId.toLowerCase()}`;
}

/**
 * Manages running Minecraft server processes for this node.
 */
export class ServerProcessManager {
  constructor(config, eventCollector = null) {
    /** @type {Map<string, ManagedServer>} serverId -> process info */
    this.servers = new Map();
    this.config = config;
    this.dataDir = config.dataDir;
    this.events = eventCollector;

    // Ensure servers directory exists
    this.serversDir = path.join(this.dataDir, "servers");
    fs.mkdirSync(this.serversDir, { recursive: true });
  }

  /**
   * Get the working directory for a server. The display name deliberately has
   * no role here: names can change and different names can sanitize to the
   * same filesystem segment.
   */
  getServerDir(serverId) {
    const serversRoot = path.resolve(this.serversDir);
    const serverDir = path.resolve(serversRoot, serverStorageKey(serverId));
    const relative = path.relative(serversRoot, serverDir);

    // Defense in depth for future changes to the storage-key format. A valid
    // UUID is already path-safe, but every filesystem mutation relies on this
    // method, so it must never return a path outside the managed root.
    if (
      !relative ||
      relative === ".." ||
      relative.startsWith(`..${path.sep}`) ||
      path.isAbsolute(relative)
    ) {
      throw new Error("Resolved server directory is outside the managed servers root");
    }

    return serverDir;
  }

  /**
   * Start a Minecraft server.
   * @param {object} instance - Server instance from Supabase
   * @returns {{ success: boolean, pid?: number, error?: string }}
   */
  start(instance) {
    const serverId = instance.id;

    // Check if already running
    if (this.servers.has(serverId)) {
      const existing = this.servers.get(serverId);
      if (existing.process && !existing.process.killed) {
        return { success: false, error: "Server is already running" };
      }
      // Clean up dead process reference
      this.servers.delete(serverId);
    }

    const serverDir = this.getServerDir(serverId, instance.name);
    fs.mkdirSync(serverDir, { recursive: true });

    // Build server jar path (convention: server.jar in server dir)
    const jarPath = path.join(serverDir, "server.jar");
    const hasJar = fs.existsSync(jarPath);

    if (!hasJar) {
      return {
        success: false,
        error: `Server jar not found at ${jarPath}. Deploy the jar first.`,
      };
    }

    // Build JVM arguments
    const memoryMb = instance.memory_mb || 2048;
    const jvmArgs = [
      `-Xms${Math.min(memoryMb, 1024)}M`,
      `-Xmx${memoryMb}M`,
      "-jar",
      "server.jar",
      "nogui",
      ...(instance.jvm_args || []),
    ];

    // Java path
    const javaPath = this.config.javaPath || "java";

    console.log(
      `[lifecycle] Starting server ${instance.name} (id=${serverId}) in ${serverDir}`
    );
    console.log(`[lifecycle] Command: ${javaPath} ${jvmArgs.join(" ")}`);

    const child = spawn(javaPath, jvmArgs, {
      cwd: serverDir,
      stdio: ["pipe", "pipe", "pipe"],
      env: {
        ...process.env,
        JAVA_HOME: process.env.JAVA_HOME || "",
      },
    });

    // Persistent console log (bounded, rotated). Recover the tail of the
    // previous session so console history and line numbering survive restarts.
    const logDir = path.join(serverDir, "logs");
    fs.mkdirSync(logDir, { recursive: true });
    const consoleLogPath = path.join(logDir, "console.jsonl");
    const recovered = recoverConsoleLog(consoleLogPath);

    const managed = {
      process: child,
      pid: child.pid,
      serverId,
      serverName: instance.name,
      startedAt: Date.now(),
      exitCode: null,
      consoleBuffer: recovered.lines,
      lineCounter: recovered.lastLineNumber,
      consoleLogPath,
      stdoutRemainder: "",
      stderrRemainder: "",
    };

    // Preserve partial lines across stream chunks. Minecraft/JVM output often
    // arrives in the middle of a line, and splitting each chunk independently
    // would corrupt console records and line numbers.
    child.stdout.on("data", (data) => appendConsoleData(managed, data, "server"));
    child.stderr.on("data", (data) => appendConsoleData(managed, data, "system"));

    // Handle exit
    child.on("exit", (code, signal) => {
      appendConsoleData(managed, "", "server", true);
      appendConsoleData(managed, "", "system", true);
      console.log(
        `[lifecycle] Server ${instance.name} exited (code=${code}, signal=${signal})`
      );
      managed.exitCode = code;
      this.servers.delete(serverId);

      if (this.events) {
        this.events.serverStopped(serverId, instance.name, `exit_code=${code},signal=${signal}`);
      }
    });

    child.on("error", (err) => {
      console.error(`[lifecycle] Server ${instance.name} error:`, err.message);
      managed.exitCode = -1;
      this.servers.delete(serverId);

      if (this.events) {
        this.events.serverError(serverId, instance.name, err.message);
      }
    });

    this.servers.set(serverId, managed);

    // Record data collection event
    if (this.events) {
      this.events.serverStarted(serverId, instance.name, child.pid);
    }

    return { success: true, pid: child.pid };
  }

  /**
   * Stop a running server.
   */
  stop(serverId) {
    const managed = this.servers.get(serverId);
    if (!managed || !managed.process) {
      return { success: false, error: "Server is not running" };
    }

    console.log(`[lifecycle] Stopping server ${managed.serverName} (pid=${managed.pid})`);

    try {
      // Send 'stop' command via stdin (graceful shutdown)
      managed.process.stdin.write("stop\n");

      // Give it 10 seconds, then force kill. The timeout is harmless after the
      // normal exit because the process is removed from the map.
      setTimeout(() => {
        if (this.servers.has(serverId) && managed.process && !managed.process.killed) {
          console.log(`[lifecycle] Force killing server ${managed.serverName}`);
          managed.process.kill("SIGTERM");
        }
      }, 10000).unref?.();

      return { success: true };
    } catch (err) {
      return { success: false, error: err.message };
    }
  }

  /**
   * Send a command to a running server's stdin.
   */
  command(serverId, command) {
    const managed = this.servers.get(serverId);
    if (!managed || !managed.process) {
      return { success: false, error: "Server is not running" };
    }

    try {
      managed.process.stdin.write(command + "\n");
      return { success: true };
    } catch (err) {
      return { success: false, error: err.message };
    }
  }

  /**
   * Get the console output buffer for a server.
   */
  getConsoleBuffer(serverId, sinceLineNumber = 0) {
    const managed = this.servers.get(serverId);
    if (!managed) return [];

    if (sinceLineNumber > 0) {
      return managed.consoleBuffer.filter(
        (line) => line.line_number > sinceLineNumber
      );
    }

    return managed.consoleBuffer;
  }

  /**
   * Check if a server is running.
   */
  isRunning(serverId) {
    const managed = this.servers.get(serverId);
    return Boolean(managed && managed.process && managed.exitCode === null && !managed.process.killed);
  }

  /**
   * Wait for a managed process to exit, then force-stop it if necessary.
   */
  waitForExit(serverId, timeoutMs = 12000) {
    const managed = this.servers.get(serverId);
    if (!managed || managed.exitCode !== null) return Promise.resolve(true);
    return new Promise((resolve) => {
      const started = Date.now();
      const poll = () => {
        if (!this.servers.has(serverId) || managed.exitCode !== null) {
          resolve(true);
          return;
        }
        if (Date.now() - started >= timeoutMs) {
          try { managed.process.kill("SIGTERM"); } catch {}
          resolve(false);
          return;
        }
        setTimeout(poll, 100);
      };
      poll();
    });
  }

  /**
   * Stop all managed servers during agent shutdown.
   */
  async shutdown() {
    const serverIds = [...this.servers.keys()];
    for (const serverId of serverIds) {
      if (!this.isRunning(serverId)) continue;
      this.stop(serverId);
      await this.waitForExit(serverId);
    }
  }

  /**
   * Create a world backup by copying the world directory.
   */
  backup(instance, label, maxBackups = 10) {
    const serverDir = this.getServerDir(instance.id, instance.name);
    const worldName = safePathSegment(instance.world_name || "world", "world");
    const worldDir = path.join(serverDir, worldName);
    const backupLabel = safePathSegment(
      label || `backup-${new Date().toISOString().replace(/[:.]/g, "-")}`,
      `backup-${Date.now()}`
    );
    const backupDir = path.join(serverDir, "backups", backupLabel);

    if (!fs.existsSync(worldDir)) {
      return { success: false, error: "World directory not found" };
    }

    try {
      fs.mkdirSync(backupDir, { recursive: true });
      copyDirRecursive(worldDir, backupDir);
      const sizeBytes = dirSize(backupDir);

      // Backup rotation: remove oldest backups beyond maxBackups
      this._rotateBackups(serverDir, maxBackups);

      return {
        success: true,
        path: backupDir,
        size_bytes: sizeBytes,
        label: backupLabel,
      };
    } catch (err) {
      return { success: false, error: err.message };
    }
  }

  /**
   * Remove oldest backups when count exceeds maxBackups.
   */
  _rotateBackups(serverDir, maxBackups) {
    const backupsDir = path.join(serverDir, "backups");
    if (!fs.existsSync(backupsDir)) return;

    try {
      const entries = fs.readdirSync(backupsDir, { withFileTypes: true })
        .filter((e) => e.isDirectory())
        .map((e) => ({
          name: e.name,
          time: fs.statSync(path.join(backupsDir, e.name)).mtime.getTime(),
        }))
        .sort((a, b) => a.time - b.time); // oldest first

      while (entries.length > maxBackups) {
        const oldest = entries.shift();
        const dirToRemove = path.join(backupsDir, oldest.name);
        console.log(`[lifecycle] Rotating old backup: ${oldest.name}`);
        fs.rmSync(dirToRemove, { recursive: true, force: true });
      }
    } catch (err) {
      console.error(`[lifecycle] Backup rotation error: ${err.message}`);
    }
  }

  /**
   * Restore a backup to the world directory.
   */
  restore(instance, backupPath) {
    const serverDir = path.resolve(this.getServerDir(instance.id, instance.name));
    const worldName = safePathSegment(instance.world_name || "world", "world");
    const worldDir = path.join(serverDir, worldName);
    const backupsDir = path.join(serverDir, "backups");
    const candidate = path.resolve(backupPath || "");
    const relative = path.relative(backupsDir, candidate);

    // Restore is allowed only from a child directory of this server's backup
    // directory. This prevents an operation payload from replacing the world
    // with arbitrary files from the host filesystem.
    if (
      !backupPath ||
      !relative ||
      relative === ".." ||
      relative.startsWith(`..${path.sep}`) ||
      path.isAbsolute(relative)
    ) {
      return { success: false, error: "Backup path must be inside this server's backups directory" };
    }
    if (!fs.existsSync(candidate) || !fs.statSync(candidate).isDirectory()) {
      return { success: false, error: "Backup path not found" };
    }

    // Never delete the live world before the replacement is complete. Build a
    // full replacement in a unique sibling directory first, then use same-root
    // renames to swap it into place. The previous world remains in a rollback
    // container until the new world is active.
    let stagedWorldDir = null;
    let rollbackContainer = null;
    let rollbackWorldDir = null;
    try {
      stagedWorldDir = fs.mkdtempSync(path.join(serverDir, ".amalgam-restore-stage-"));
      copyDirRecursive(candidate, stagedWorldDir);

      if (fs.existsSync(worldDir)) {
        rollbackContainer = fs.mkdtempSync(
          path.join(serverDir, ".amalgam-restore-rollback-")
        );
        rollbackWorldDir = path.join(rollbackContainer, worldName);
        fs.renameSync(worldDir, rollbackWorldDir);
      }

      // Both paths are direct children of serverDir, so this is a same-volume
      // directory rename rather than a copy-overwrite operation.
      fs.renameSync(stagedWorldDir, worldDir);
      stagedWorldDir = null;

      if (rollbackContainer) {
        try {
          fs.rmSync(rollbackContainer, { recursive: true, force: true });
        } catch (cleanupError) {
          // The replacement is already live. Retaining an old-world rollback
          // is safer than reporting a failed restore or deleting it blindly.
          console.warn(`[lifecycle] Restore rollback cleanup failed: ${cleanupError.message}`);
        }
      }
      return { success: true };
    } catch (err) {
      let recoveryError = null;

      // If the old world was moved aside but the replacement was not promoted,
      // put it back. Do not overwrite a destination that exists unexpectedly:
      // in that case leave the rollback intact and give the caller its path.
      if (
        rollbackWorldDir &&
        fs.existsSync(rollbackWorldDir) &&
        !fs.existsSync(worldDir)
      ) {
        try {
          fs.renameSync(rollbackWorldDir, worldDir);
          try {
            fs.rmdirSync(rollbackContainer);
          } catch {
            // An empty temporary rollback container is harmless.
          }
          rollbackContainer = null;
          rollbackWorldDir = null;
        } catch (rollbackErr) {
          recoveryError = rollbackErr;
        }
      }

      if (stagedWorldDir && fs.existsSync(stagedWorldDir)) {
        try {
          fs.rmSync(stagedWorldDir, { recursive: true, force: true });
        } catch {
          // Preserve an incomplete staging directory for operator inspection
          // rather than letting cleanup hide the original failure.
        }
      }

      const message = err instanceof Error ? err.message : String(err);
      const result = { success: false, error: message };
      if (rollbackWorldDir && fs.existsSync(rollbackWorldDir)) {
        result.recovery_path = rollbackWorldDir;
      }
      if (recoveryError) {
        result.error += `; rollback failed: ${recoveryError.message}`;
      }
      return result;
    }
  }

  /**
   * Write server.properties file.
   */
  applySettings(instance, settings) {
    const serverDir = this.getServerDir(instance.id, instance.name);
    const propsPath = path.join(serverDir, "server.properties");
    const props = {
      ...SERVER_PROPERTY_DEFAULTS,
      "server-port": instance.port || SERVER_PROPERTY_DEFAULTS["server-port"],
      "max-players": instance.max_players || SERVER_PROPERTY_DEFAULTS["max-players"],
      "motd": instance.motd || SERVER_PROPERTY_DEFAULTS.motd,
      "gamemode": instance.gamemode || SERVER_PROPERTY_DEFAULTS.gamemode,
      "difficulty": instance.difficulty || SERVER_PROPERTY_DEFAULTS.difficulty,
      "whitelist": instance.whitelist_enabled || false,
      "online-mode": instance.online_mode !== false,
      "level-name": instance.world_name || SERVER_PROPERTY_DEFAULTS["level-name"],
    };

    try {
      applyPropertyOverrides(props, settings, "operation settings");

      // Convert the explicitly supported sp_<property> template metadata.
      // Other metadata remains metadata; it must not become a server property.
      if (instance.metadata && typeof instance.metadata === "object") {
        const templateProperties = Object.create(null);
        for (const [key, value] of Object.entries(instance.metadata)) {
          if (key.startsWith("sp_")) templateProperties[key.slice(3)] = value;
        }
        applyPropertyOverrides(props, templateProperties, "template metadata");
      }

      // Defense in depth: the world folder name is a filesystem path on disk,
      // so it must never come from an untrusted payload unsanitized. A hostile
      // level-name could otherwise make the server write its world outside the
      // server directory (and outside the backup/restore sandbox).
      props["level-name"] = safePathSegment(props["level-name"] || "", "world");
    } catch (err) {
      return { success: false, error: err instanceof Error ? err.message : String(err) };
    }

    const lines = Object.entries(props)
      .map(([key, value]) => `${key}=${String(value)}`)
      .join("\n");
    const tempPath = `${propsPath}.amalgam-tmp-${process.pid}-${Date.now()}`;

    try {
      fs.mkdirSync(serverDir, { recursive: true });
      fs.writeFileSync(tempPath, lines + "\n", "utf-8");
      // Renaming a fully written sibling prevents an interrupted write from
      // leaving a partially constructed server.properties in the live slot.
      fs.renameSync(tempPath, propsPath);
      return { success: true };
    } catch (err) {
      try { fs.rmSync(tempPath, { force: true }); } catch {}
      return { success: false, error: err instanceof Error ? err.message : String(err) };
    }
  }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

function applyPropertyOverrides(props, overrides, source) {
  if (overrides === null || typeof overrides !== "object" || Array.isArray(overrides)) {
    throw new Error(`${source} must be an object`);
  }

  for (const [key, value] of Object.entries(overrides)) {
    if (!ALLOWED_SERVER_PROPERTIES.has(key)) {
      throw new Error(`Unsupported server property in ${source}: ${key}`);
    }
    if (!["string", "number", "boolean"].includes(typeof value) ||
        (typeof value === "number" && !Number.isFinite(value))) {
      throw new Error(`Server property ${key} in ${source} must be a finite primitive value`);
    }
    if (/[\r\n\0]/.test(String(value))) {
      throw new Error(`Server property ${key} in ${source} must not contain line breaks or NUL`);
    }
    props[key] = value;
  }
}

export function appendConsoleData(managed, data, source, flushRemainder = false) {
  const remainderKey = source === "system" ? "stderrRemainder" : "stdoutRemainder";
  const combined = managed[remainderKey] + String(data || "");
  const parts = combined.split(/\r?\n/);
  // When not flushing, pop() removes the trailing (incomplete) segment; when
  // flushing, the remainder is discarded. In both cases everything left in
  // `parts` is a complete line — slicing off another element here would
  // silently drop the last complete line of every chunk.
  managed[remainderKey] = flushRemainder ? "" : (parts.pop() || "");
  const completeLines = parts;
  for (const line of completeLines) {
    if (!line) continue;
    managed.lineCounter++;
    const entry = {
      line_number: managed.lineCounter,
      level: source === "system" ? "error" : classifyLine(line),
      source,
      content: stripAnsi(line),
      raw: line,
      timestamp: new Date().toISOString(),
    };
    managed.consoleBuffer.push(entry);
    // Persist every complete line to the bounded, rotating JSONL log.
    // appendConsoleLine is best-effort and never throws into the caller.
    if (managed.consoleLogPath) {
      appendConsoleLine(managed.consoleLogPath, {}, entry);
    }
  }
  if (managed.consoleBuffer.length > 1000) managed.consoleBuffer = managed.consoleBuffer.slice(-500);
}

function classifyLine(line) {
  const lower = line.toLowerCase();
  if (
    lower.includes("/error]") ||
    lower.includes("[error]") ||
    lower.includes("exception") ||
    lower.includes("fatal") ||
    lower.includes("crash") ||
    lower.includes("error:") ||
    lower.includes("outofmemoryerror")
  ) {
    return "error";
  }
  if (
    lower.includes("/warn]") ||
    lower.includes("[warn]") ||
    lower.includes("[warning]") ||
    lower.includes("deprecated")
  ) {
    return "warn";
  }
  return "info";
}

function stripAnsi(str) {
  // Remove ANSI escape sequences (color codes)
  // eslint-disable-next-line no-control-regex
  return str.replace(/\x1B\[[0-9;]*[a-zA-Z]/g, "");
}

function safePathSegment(value, fallback) {
  const normalized = String(value || "").replace(/[^a-zA-Z0-9._-]/g, "_");
  if (!normalized || normalized === "." || normalized === "..") return fallback;
  return normalized.slice(0, 128);
}

function copyDirRecursive(src, dest) {
  fs.mkdirSync(dest, { recursive: true });
  const entries = fs.readdirSync(src, { withFileTypes: true });
  for (const entry of entries) {
    const srcPath = path.join(src, entry.name);
    const destPath = path.join(dest, entry.name);
    if (entry.isDirectory()) {
      copyDirRecursive(srcPath, destPath);
    } else {
      fs.copyFileSync(srcPath, destPath);
    }
  }
}

function dirSize(dir) {
  let total = 0;
  try {
    const entries = fs.readdirSync(dir, { withFileTypes: true });
    for (const entry of entries) {
      const fullPath = path.join(dir, entry.name);
      if (entry.isDirectory()) {
        total += dirSize(fullPath);
      } else {
        total += fs.statSync(fullPath).size;
      }
    }
  } catch {
    // Ignore errors
  }
  return total;
}
