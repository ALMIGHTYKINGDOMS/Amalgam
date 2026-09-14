// Minecraft server process management.
// Spawns Java processes, captures stdout/stderr, manages lifecycle.

import { spawn } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { appendConsoleLine, recoverConsoleLog } from "./console-log.js";

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
   * Get the working directory for a server.
   */
  getServerDir(serverId, serverName) {
    const safeName = (serverName || serverId).replace(/[^a-zA-Z0-9_-]/g, "_");
    return path.join(this.serversDir, safeName);
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
    // directory. This prevents an operation payload from deleting the world
    // and copying arbitrary files from the host filesystem.
    if (!backupPath || relative.startsWith("..") || path.isAbsolute(relative)) {
      return { success: false, error: "Backup path must be inside this server's backups directory" };
    }
    if (!fs.existsSync(candidate) || !fs.statSync(candidate).isDirectory()) {
      return { success: false, error: "Backup path not found" };
    }

    try {
      if (fs.existsSync(worldDir)) fs.rmSync(worldDir, { recursive: true, force: true });
      copyDirRecursive(candidate, worldDir);
      return { success: true };
    } catch (err) {
      return { success: false, error: err.message };
    }
  }

  /**
   * Write server.properties file.
   */
  applySettings(instance, settings) {
    const serverDir = this.getServerDir(instance.id, instance.name);
    const propsPath = path.join(serverDir, "server.properties");

    const defaults = {
      "server-port": instance.port || 25565,
      "max-players": instance.max_players || 20,
      "motd": instance.motd || "An Amalgam Server",
      "gamemode": instance.gamemode || "survival",
      "difficulty": instance.difficulty || "normal",
      "whitelist": instance.whitelist_enabled || false,
      "online-mode": instance.online_mode !== false,
      "level-name": safePathSegment(instance.world_name || "world", "world"),
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
    };

    // Merge provided settings
    const props = { ...defaults, ...settings };

    // Convert JSONB server_properties from template
    if (instance.metadata && typeof instance.metadata === "object") {
      for (const [key, value] of Object.entries(instance.metadata)) {
        if (key.startsWith("sp_")) {
          props[key.slice(3)] = value;
        }
      }
    }

    // Defense in depth: the world folder name is a filesystem path on disk,
    // so it must never come from an untrusted payload unsanitized. A hostile
    // level-name could otherwise make the server write its world outside the
    // server directory (and outside the backup/restore sandbox).
    props["level-name"] = safePathSegment(props["level-name"] || "", "world");

    const lines = Object.entries(props)
      .map(([key, value]) => {
        const val = typeof value === "boolean" ? String(value) : String(value);
        return `${key}=${val}`;
      })
      .join("\n");

    try {
      fs.writeFileSync(propsPath, lines + "\n", "utf-8");
      return { success: true };
    } catch (err) {
      return { success: false, error: err.message };
    }
  }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

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
