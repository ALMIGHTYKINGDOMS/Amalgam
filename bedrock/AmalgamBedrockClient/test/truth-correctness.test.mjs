import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import test from "node:test";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const require = createRequire(import.meta.url);
const { renderRuntimeMetadata } = require(path.join(root, "tools", "runtime-metadata.js"));

function read(relative) {
  return fs.readFileSync(path.join(root, relative), "utf8");
}

test("package, manifests, and runtime metadata share one release contract", () => {
  const pkg = JSON.parse(read("package.json"));
  const behavior = JSON.parse(read("behavior_pack/manifest.json"));
  const resource = JSON.parse(read("resource_pack/manifest.json"));
  const expectedVersion = pkg.version.split("-")[0].split(".").map(Number);
  const expectedMinimumBedrock = pkg.amalgam.minimum_bedrock.split(".").map(Number);

  for (const manifest of [behavior, resource]) {
    assert.deepEqual(manifest.header.version, expectedVersion);
    assert.deepEqual(manifest.header.min_engine_version, expectedMinimumBedrock);
    for (const module of manifest.modules) assert.deepEqual(module.version, expectedVersion);
  }
  assert.deepEqual(behavior.dependencies.find((dependency) => dependency.uuid).version, expectedVersion);
  assert.equal(behavior.dependencies.find((dependency) => dependency.module_name === "@minecraft/server").version, pkg.amalgam.script_api);
  assert.equal(behavior.dependencies.find((dependency) => dependency.module_name === "@minecraft/server-ui").version, pkg.amalgam.server_ui_api);
  assert.equal(read("behavior_pack/scripts/util/runtime_metadata.js").replace(/\r\n/g, "\n"), renderRuntimeMetadata(pkg));
});

test("per-player runtime state does not share settings, session, or menu state", async () => {
  const runtimeMetadataUrl = `data:text/javascript;charset=utf-8,${encodeURIComponent(read("behavior_pack/scripts/util/runtime_metadata.js"))}`;
  const stateSource = read("behavior_pack/scripts/util/state.js").replace("\"./runtime_metadata.js\"", JSON.stringify(runtimeMetadataUrl));
  const state = await import(`data:text/javascript;charset=utf-8,${encodeURIComponent(stateSource)}`);
  const alice = { id: "truth-test-alice" };
  const bob = { id: "truth-test-bob" };

  state.clearPlayerState(alice);
  state.clearPlayerState(bob);
  state.updatePlayerSettings(alice, { hudEnabled: false, notificationsEnabled: false });
  state.updatePlayerSession(alice, { playerName: "Alice", worldName: "Alice world", dimension: "minecraft:overworld", startedAt: 1 });
  state.setPlayerMenuOpen(alice, true);

  assert.equal(state.getPlayerSettings(alice).hudEnabled, false);
  assert.equal(state.getPlayerSettings(bob).hudEnabled, true);
  assert.equal(state.getPlayerSettings(bob).notificationsEnabled, true);
  assert.equal(state.getPlayerSession(alice).worldName, "Alice world");
  assert.equal(state.getPlayerSession(bob).worldName, "");
  assert.equal(state.isPlayerMenuOpen(alice), true);
  assert.equal(state.isPlayerMenuOpen(bob), false);

  state.clearPlayerState(alice.id);
  assert.equal(state.getPlayerSettings(alice).hudEnabled, true);
  assert.equal(state.getPlayerSession(alice).worldName, "");
});

test("HUD and storage source reflect supported per-player action-bar behavior", () => {
  const menu = read("behavior_pack/scripts/menu.js");
  const storage = read("behavior_pack/scripts/storage.js");

  assert.match(menu, /Minecraft controls its position, size, and opacity/);
  assert.match(menu, /only show or hide the HUD/);
  assert.doesNotMatch(menu, /hudPreset|hudScale|hudOpacity/);
  assert.match(storage, /player\?\.getDynamicProperty/);
  assert.match(storage, /player\.setDynamicProperty/);
  assert.doesNotMatch(storage, /world\.getDynamicProperty|world\.setDynamicProperty/);
});

test("the normal Bedrock validator accepts the synchronized contract", () => {
  execFileSync(process.execPath, [path.join(root, "tools", "validate.js")], { cwd: root, stdio: "pipe" });
});
