import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { ServerProcessManager } from "./server-lifecycle.js";

function tempConfig() {
  const dataDir = fs.mkdtempSync(path.join(os.tmpdir(), "amalgam-runtime-test-"));
  return { dataDir, javaPath: "java" };
}

test("restore rejects paths outside the server backup directory", () => {
  const config = tempConfig();
  const manager = new ServerProcessManager(config);
  const instance = { id: "server-1", name: "Test Server", world_name: "world" };
  const outside = path.join(config.dataDir, "outside");
  fs.mkdirSync(outside, { recursive: true });
  assert.equal(manager.restore(instance, outside).success, false);
  fs.rmSync(config.dataDir, { recursive: true, force: true });
});

test("backup rotation keeps the configured number of backups", () => {
  const config = tempConfig();
  const manager = new ServerProcessManager(config);
  const instance = { id: "server-1", name: "Test Server", world_name: "world" };
  const serverDir = manager.getServerDir(instance.id, instance.name);
  fs.mkdirSync(path.join(serverDir, "world"), { recursive: true });
  fs.writeFileSync(path.join(serverDir, "world", "level.dat"), "test");
  for (const label of ["one", "two", "three"]) manager.backup(instance, label, 2);
  const backups = fs.readdirSync(path.join(serverDir, "backups"));
  assert.equal(backups.length, 2);
  fs.rmSync(config.dataDir, { recursive: true, force: true });
});
