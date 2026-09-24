import test from "node:test";
import assert from "node:assert/strict";
import crypto from "node:crypto";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { ServerProcessManager } from "./server-lifecycle.js";
import {
  fetchPendingOperations,
  handleInstallJar,
  handleRestart,
  handleRestore,
  processOperation,
} from "./operations.js";

const SERVER_ID = "33333333-3333-4333-8333-333333333333";
const NODE_ID = "44444444-4444-4444-8444-444444444444";

function tempManager() {
  const dataDir = fs.mkdtempSync(path.join(os.tmpdir(), "amalgam-operations-security-"));
  return new ServerProcessManager({ dataDir, javaPath: "java" });
}

function instance() {
  return { id: SERVER_ID, name: "Security Test", world_name: "world" };
}

test("server jar deployment requires a pinned digest and preserves the prior jar on mismatch", async () => {
  const manager = tempManager();
  const srv = instance();
  const originalFetch = globalThis.fetch;
  let fetchCalls = 0;
  const expected = Buffer.from("known good server jar bytes");
  const expectedHash = crypto.createHash("sha256").update(expected).digest("hex");

  try {
    globalThis.fetch = async () => {
      fetchCalls += 1;
      return new Response(expected, {
        status: 200,
        headers: { "content-length": String(expected.length) },
      });
    };

    const missingDigest = await handleInstallJar(manager, srv, { params: { url: "https://example.com/server.jar" } });
    assert.equal(missingDigest.success, false);
    assert.match(missingDigest.error, /sha-256 digest is required/i);
    assert.equal(fetchCalls, 0, "a missing digest must be rejected before network I/O");

    const installed = await handleInstallJar(manager, srv, {
      params: { url: "https://example.com/server.jar", sha256: expectedHash },
    });
    assert.equal(installed.success, true);
    assert.equal(installed.sha256, expectedHash);
    const jarPath = path.join(manager.getServerDir(srv.id), "server.jar");
    assert.deepEqual(fs.readFileSync(jarPath), expected);

    globalThis.fetch = async () => new Response(Buffer.from("tampered bytes"), {
      status: 200,
      headers: { "content-length": String("tampered bytes".length) },
    });
    const mismatch = await handleInstallJar(manager, srv, {
      params: { url: "https://example.com/server.jar", sha256: expectedHash },
    });
    assert.equal(mismatch.success, false);
    assert.match(mismatch.error, /SHA-256 verification failed/i);
    assert.deepEqual(fs.readFileSync(jarPath), expected, "bad bytes never replace a verified jar");
    assert.equal(fs.existsSync(`${jarPath}.part`), false, "failed staging data is removed");

    const privateHost = await handleInstallJar(manager, srv, {
      params: { url: "https://127.0.0.1/server.jar", sha256: expectedHash },
    });
    assert.equal(privateHost.success, false);
    assert.equal(fetchCalls, 1, "private-address input is rejected before fetch");
  } finally {
    globalThis.fetch = originalFetch;
    fs.rmSync(manager.dataDir, { recursive: true, force: true });
  }
});

test("restart and restore refuse to operate through a still-running process", async () => {
  const srv = instance();
  const restartManager = {
    isRunning: () => true,
    stop: () => ({ success: true }),
    waitForExit: async () => false,
    start: () => {
      throw new Error("start must not run before exit");
    },
  };
  const restart = await handleRestart({}, {}, {}, restartManager, srv, {});
  assert.equal(restart.success, false);
  assert.match(restart.error, /did not exit/i);

  let restoreCalled = false;
  const restoreManager = {
    isRunning: () => true,
    restore: () => {
      restoreCalled = true;
      return { success: true };
    },
  };
  const restore = await handleRestore(restoreManager, srv, { params: { backup_path: "C:/ignored" } });
  assert.equal(restore.success, false);
  assert.match(restore.error, /stop the server/i);
  assert.equal(restoreCalled, false);
});

test("node queue polling and dispatch reject cross-node work before control-plane mutation", async () => {
  const calls = [];
  const query = {
    select() { calls.push("select"); return this; },
    eq(column, value) { calls.push(`eq:${column}:${value}`); return this; },
    order() { calls.push("order"); return this; },
    limit() { calls.push("limit"); return Promise.resolve({ data: [], error: null }); },
  };
  const supabase = {
    from(table) {
      assert.equal(table, "server_operations");
      return query;
    },
    rpc() {
      throw new Error("cross-node work must not call an RPC");
    },
  };
  const node = { id: NODE_ID };
  const result = await fetchPendingOperations(supabase, node, 4);
  assert.deepEqual(result, { data: [], error: null });
  assert.ok(calls.includes(`eq:node_id:${NODE_ID}`));

  await processOperation(supabase, node, { nodeSecret: "unused" }, {}, {
    id: "op-1",
    server_id: SERVER_ID,
    node_id: "55555555-5555-4555-8555-555555555555",
    operation: "delete",
  });
});
