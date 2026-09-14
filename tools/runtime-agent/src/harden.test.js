import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { ServerProcessManager, appendConsoleData } from "./server-lifecycle.js";
import { checkTimeouts } from "./operations.js";

function tempConfig() {
  const dataDir = fs.mkdtempSync(path.join(os.tmpdir(), "amalgam-harden-"));
  return { dataDir, javaPath: "java" };
}

function instance(id = "server-1", name = "Test Server") {
  return { id, name, world_name: "world" };
}

// ---------------------------------------------------------------------------
// checkTimeouts — no unbounded operation may remain
// ---------------------------------------------------------------------------

test("checkTimeouts fails operations that ran past their deadline", async () => {
  const rpcCalls = [];
  const old = Date.now() - 100_000;
  const stuckOps = [
    { id: "op-1", operation: "backup", server_id: "srv-1", claimed_at: null, started_at: new Date(old).toISOString(), timeout_seconds: 30 },
    { id: "op-2", operation: "start", server_id: "srv-2", claimed_at: new Date(old).toISOString(), started_at: new Date(old).toISOString(), timeout_seconds: 60 },
  ];
  const supabase = {
    from: () => ({
      select: () => ({
        in: () => ({
          eq: () => Promise.resolve({ data: stuckOps, error: null }),
        }),
      }),
    }),
    rpc: async (name, args) => {
      rpcCalls.push({ name, args });
      return { data: null, error: null };
    },
  };
  await checkTimeouts(supabase, { id: "node-1" }, {});
  const completes = rpcCalls.filter((c) => c.name === "complete_operation");
  assert.equal(completes.length, 2);
  assert.ok(completes.every((c) => c.args.p_success === false));
  assert.ok(completes.every((c) => c.args.p_result.timed_out === true));
});

test("checkTimeouts leaves fresh operations alone", async () => {
  const rpcCalls = [];
  const fresh = [
    { id: "op-new", operation: "start", server_id: "srv-1", claimed_at: null, started_at: new Date().toISOString(), timeout_seconds: 300 },
  ];
  const supabase = {
    from: () => ({
      select: () => ({
        in: () => ({
          eq: () => Promise.resolve({ data: fresh, error: null }),
        }),
      }),
    }),
    rpc: async (name, args) => {
      rpcCalls.push({ name, args });
      return { data: null, error: null };
    },
  };
  await checkTimeouts(supabase, { id: "node-1" }, {});
  assert.equal(rpcCalls.length, 0);
});

// ---------------------------------------------------------------------------
// Process lifecycle — failures must not leave stale state
// ---------------------------------------------------------------------------

test("start without a deployed jar fails safely", () => {
  const config = tempConfig();
  const manager = new ServerProcessManager(config);
  const result = manager.start(instance());
  assert.equal(result.success, false);
  assert.match(result.error, /jar not found/);
  assert.equal(manager.isRunning("server-1"), false);
  fs.rmSync(config.dataDir, { recursive: true, force: true });
});

test("stop and command on an unknown server fail cleanly", () => {
  const config = tempConfig();
  const manager = new ServerProcessManager(config);
  assert.equal(manager.stop("ghost").success, false);
  assert.equal(manager.command("ghost", "list").success, false);
  assert.deepEqual(manager.getConsoleBuffer("ghost"), []);
  fs.rmSync(config.dataDir, { recursive: true, force: true });
});

test("a process that dies immediately is cleaned from the running set", async () => {
  const config = tempConfig();
  config.javaPath = process.execPath; // node, which rejects "-jar" and exits 9
  const manager = new ServerProcessManager(config);
  const srv = instance("crashy", "Crashy");
  const dir = manager.getServerDir(srv.id, srv.name);
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(path.join(dir, "server.jar"), "this is not a jar");
  const start = manager.start(srv);
  assert.equal(start.success, true);
  // Node exits with code 9 on the bad "-jar" option almost immediately.
  await new Promise((resolve) => setTimeout(resolve, 1200));
  assert.equal(manager.isRunning("crashy"), false);
  assert.equal(manager.servers.has("crashy"), false, "dead process removed from the map");
  fs.rmSync(config.dataDir, { recursive: true, force: true });
});

test("waitForExit force-kills a process that ignores the stop command", async () => {
  const config = tempConfig();
  const manager = new ServerProcessManager(config);
  const fake = {
    process: { killed: false, kill: () => {} },
    pid: 4242,
    serverId: "srv-1",
    exitCode: null,
  };
  manager.servers.set("srv-1", fake);
  const ok = await manager.waitForExit("srv-1", 300);
  assert.equal(ok, false, "force-stop path returns false when the process would not exit");
  fs.rmSync(config.dataDir, { recursive: true, force: true });
});

// ---------------------------------------------------------------------------
// Console: partial-line reconstruction, bounds, persistence
// ---------------------------------------------------------------------------

function fakeManaged(logPath) {
  return {
    lineCounter: 0,
    consoleBuffer: [],
    consoleLogPath: logPath,
    stdoutRemainder: "",
    stderrRemainder: "",
  };
}

test("partial lines split across chunks are reconstructed in order", () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "amalgam-partial-"));
  const logPath = path.join(dir, "console.jsonl");
  const m = fakeManaged(logPath);
  appendConsoleData(m, "Starting minecraft", "server");
  appendConsoleData(m, " server version 1.21", "server");
  appendConsoleData(m, "\n[12:00:00] [main/INFO]: Done", "server");
  appendConsoleData(m, " (1.2s)! For help, type \"help\"\n", "server");
  assert.equal(m.lineCounter, 2);
  assert.equal(m.consoleBuffer[0].content, "Starting minecraft server version 1.21");
  assert.equal(m.consoleBuffer[1].content, '[12:00:00] [main/INFO]: Done (1.2s)! For help, type "help"');
  fs.rmSync(dir, { recursive: true, force: true });
});

test("console buffer stays bounded under a flood", () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "amalgam-flood-"));
  const logPath = path.join(dir, "console.jsonl");
  const m = fakeManaged(logPath);
  for (let i = 0; i < 2000; i++) appendConsoleData(m, `line ${i}\n`, "server");
  assert.ok(m.consoleBuffer.length <= 1000, `buffer bounded (got ${m.consoleBuffer.length})`);
  assert.ok(m.consoleBuffer.length >= 500);
  fs.rmSync(dir, { recursive: true, force: true });
});

test("exit flush emits a final partial line and recovers on restart", () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "amalgam-flush-"));
  const logPath = path.join(dir, "console.jsonl");
  const m = fakeManaged(logPath);
  appendConsoleData(m, "Server thread/INFO]: Stopping", "server");
  appendConsoleData(m, " server\n", "server");
  appendConsoleData(m, "unfinished tail without newline", "server");
  appendConsoleData(m, "", "server", true); // exit flush
  assert.equal(m.lineCounter, 2);
  assert.equal(m.consoleBuffer[0].content, "Server thread/INFO]: Stopping server");
  assert.equal(m.consoleBuffer[1].content, "unfinished tail without newline");
  fs.rmSync(dir, { recursive: true, force: true });
});

// ---------------------------------------------------------------------------
// Path safety
// ---------------------------------------------------------------------------

test("server directory names are sanitized (no path escape)", () => {
  const config = tempConfig();
  const manager = new ServerProcessManager(config);
  const evil = manager.getServerDir("srv-1", "../..\\..\\evil");
  const resolved = path.resolve(evil);
  const base = path.resolve(config.dataDir, "servers");
  assert.equal(path.relative(base, resolved).startsWith(".."), false);
  fs.rmSync(config.dataDir, { recursive: true, force: true });
});

test("applySettings sanitizes level-name and writes server.properties", () => {
  const config = tempConfig();
  const manager = new ServerProcessManager(config);
  const srv = instance("srv-1", "Test Server");
  const dir = manager.getServerDir(srv.id, srv.name);
  fs.mkdirSync(dir, { recursive: true });
  const result = manager.applySettings(srv, {
    "level-name": "../../etc/passwd",
    "max-players": 7,
  });
  assert.equal(result.success, true);
  const props = fs.readFileSync(path.join(dir, "server.properties"), "utf-8");
  // The world folder name must not contain path separators or a parent
  // traversal segment after sanitization.
  const levelName = props.match(/^level-name=(.*)$/m)[1];
  assert.ok(!/[\\/]/.test(levelName), "no path separators survive sanitization");
  assert.notEqual(levelName, "..");
  assert.notEqual(levelName, ".");
  assert.match(props, /^max-players=7/m);
  assert.match(props, /^server-port=25565/m);
  fs.rmSync(config.dataDir, { recursive: true, force: true });
});

test("restore rejects a backup path outside the backups directory", () => {
  const config = tempConfig();
  const manager = new ServerProcessManager(config);
  const srv = instance();
  const serverDir = manager.getServerDir(srv.id, srv.name);
  fs.mkdirSync(path.join(serverDir, "world"), { recursive: true });
  fs.writeFileSync(path.join(serverDir, "world", "level.dat"), "data");
  const outside = path.join(config.dataDir, "elsewhere");
  fs.mkdirSync(outside, { recursive: true });
  assert.equal(manager.restore(srv, outside).success, false);
  assert.equal(manager.restore(srv, path.join(config.dataDir, "..", "..", "tmp", "x")).success, false);
  fs.rmSync(config.dataDir, { recursive: true, force: true });
});

test("backup rotation keeps the configured number of backups", () => {
  const config = tempConfig();
  const manager = new ServerProcessManager(config);
  const srv = instance();
  const serverDir = manager.getServerDir(srv.id, srv.name);
  fs.mkdirSync(path.join(serverDir, "world"), { recursive: true });
  fs.writeFileSync(path.join(serverDir, "world", "level.dat"), "data");
  for (const label of ["one", "two", "three", "four"]) manager.backup(srv, label, 3);
  const backups = fs.readdirSync(path.join(serverDir, "backups")).length;
  assert.equal(backups, 3);
  fs.rmSync(config.dataDir, { recursive: true, force: true });
});
