import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { ServerProcessManager } from "./server-lifecycle.js";

const SERVER_ID = "11111111-1111-4111-8111-111111111111";

function tempConfig() {
  const dataDir = fs.mkdtempSync(path.join(os.tmpdir(), "amalgam-runtime-test-"));
  return { dataDir, javaPath: "java" };
}

function seedRestoreFixture(manager, instance) {
  const serverDir = manager.getServerDir(instance.id, instance.name);
  const worldDir = path.join(serverDir, instance.world_name);
  const backupDir = path.join(serverDir, "backups", "restore-source");
  fs.mkdirSync(worldDir, { recursive: true });
  fs.mkdirSync(backupDir, { recursive: true });
  fs.writeFileSync(path.join(worldDir, "level.dat"), "live-world");
  fs.writeFileSync(path.join(backupDir, "level.dat"), "backup-world");
  fs.writeFileSync(path.join(backupDir, "new-file.txt"), "from-backup");
  return { serverDir, worldDir, backupDir };
}

function restoreArtifacts(serverDir) {
  return fs.readdirSync(serverDir).filter((name) =>
    name.startsWith(".amalgam-restore-stage-") ||
    name.startsWith(".amalgam-restore-rollback-")
  );
}

test("restore rejects paths outside the server backup directory", () => {
  const config = tempConfig();
  const manager = new ServerProcessManager(config);
  const instance = { id: SERVER_ID, name: "Test Server", world_name: "world" };
  const outside = path.join(config.dataDir, "outside");
  fs.mkdirSync(outside, { recursive: true });
  assert.equal(manager.restore(instance, outside).success, false);
  fs.rmSync(config.dataDir, { recursive: true, force: true });
});

test("backup rotation keeps the configured number of backups", () => {
  const config = tempConfig();
  const manager = new ServerProcessManager(config);
  const instance = { id: SERVER_ID, name: "Test Server", world_name: "world" };
  const serverDir = manager.getServerDir(instance.id, instance.name);
  fs.mkdirSync(path.join(serverDir, "world"), { recursive: true });
  fs.writeFileSync(path.join(serverDir, "world", "level.dat"), "test");
  for (const label of ["one", "two", "three"]) manager.backup(instance, label, 2);
  const backups = fs.readdirSync(path.join(serverDir, "backups"));
  assert.equal(backups.length, 2);
  fs.rmSync(config.dataDir, { recursive: true, force: true });
});

test("restore stages the backup before replacing a live world", () => {
  const config = tempConfig();
  const manager = new ServerProcessManager(config);
  const instance = { id: SERVER_ID, name: "Test Server", world_name: "world" };
  const { serverDir, worldDir, backupDir } = seedRestoreFixture(manager, instance);

  const result = manager.restore(instance, backupDir);
  assert.equal(result.success, true);
  assert.equal(fs.readFileSync(path.join(worldDir, "level.dat"), "utf-8"), "backup-world");
  assert.equal(fs.readFileSync(path.join(worldDir, "new-file.txt"), "utf-8"), "from-backup");
  assert.deepEqual(restoreArtifacts(serverDir), []);
  fs.rmSync(config.dataDir, { recursive: true, force: true });
});

test("restore preserves the live world when staging copy fails", () => {
  const config = tempConfig();
  const manager = new ServerProcessManager(config);
  const instance = { id: SERVER_ID, name: "Test Server", world_name: "world" };
  const { serverDir, worldDir, backupDir } = seedRestoreFixture(manager, instance);
  const originalCopyFile = fs.copyFileSync;

  fs.copyFileSync = (source, destination, mode) => {
    if (path.resolve(source) === path.join(backupDir, "level.dat")) {
      throw new Error("forced staging copy failure");
    }
    return originalCopyFile(source, destination, mode);
  };
  try {
    const result = manager.restore(instance, backupDir);
    assert.equal(result.success, false);
    assert.match(result.error, /forced staging copy failure/);
    assert.equal(fs.readFileSync(path.join(worldDir, "level.dat"), "utf-8"), "live-world");
    assert.equal(fs.existsSync(path.join(worldDir, "new-file.txt")), false);
    assert.deepEqual(restoreArtifacts(serverDir), []);
  } finally {
    fs.copyFileSync = originalCopyFile;
    fs.rmSync(config.dataDir, { recursive: true, force: true });
  }
});

test("restore rolls the live world back when staged promotion fails", () => {
  const config = tempConfig();
  const manager = new ServerProcessManager(config);
  const instance = { id: SERVER_ID, name: "Test Server", world_name: "world" };
  const { serverDir, worldDir, backupDir } = seedRestoreFixture(manager, instance);
  const originalRename = fs.renameSync;

  fs.renameSync = (source, destination) => {
    if (
      path.basename(source).startsWith(".amalgam-restore-stage-") &&
      path.resolve(destination) === path.resolve(worldDir)
    ) {
      throw new Error("forced staged promotion failure");
    }
    return originalRename(source, destination);
  };
  try {
    const result = manager.restore(instance, backupDir);
    assert.equal(result.success, false);
    assert.match(result.error, /forced staged promotion failure/);
    assert.equal(fs.readFileSync(path.join(worldDir, "level.dat"), "utf-8"), "live-world");
    assert.equal(fs.existsSync(path.join(worldDir, "new-file.txt")), false);
    assert.deepEqual(restoreArtifacts(serverDir), []);
  } finally {
    fs.renameSync = originalRename;
    fs.rmSync(config.dataDir, { recursive: true, force: true });
  }
});
