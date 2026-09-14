# Architecture

## Overview

```
amalgam_launcher.exe  (C++/ImGui)
      |  resolves version/loader/java/libs/assets, verifies sha1/size, downloads
      |  launches: java -agentpath:amalgam.dll ... -Damalgam.dll.path=... -cp <libs> <bridge mod in instance mods/>
      v
minecraft (JVM)
  amalgam.dll  (native, injected via -agentpath)
      |  JNI/JVMTI bridge -> native action pipeline
      ^
      |  Java bridge mod (amalgam-<loader>-<mc>.jar) drains actions at tick start and publishes bounded sidebar telemetry
  aml::bridge  (JNI exports)
```

## Launcher

### Version/loader resolution (`model.cpp`)

- Mojang version manifest filtered to rank >= 1.12 (`major*1000 + minor*10 + patch`, e.g. 1.21.11 → 1221), newest first.
- **Fabric/Quilt**: profile JSON is a delta with `inheritsFrom: <mc>`. `model::merge_inherited` fetches the base version JSON, parses base libs/assets/java/client, then overlays `mainClass`, args, and libraries (dedup). Profile libs are maven coordinates → URLs derived from the artifact path.
- **Forge/NeoForge**: Maven profile JSONs 404, so the launcher uses the **installer route**: download `<installer>-installer.jar`, run it with `--installClient <instance>` (300 s timeout), parse the produced `<instance>/versions/*/*.json`, and use the local patched jar as the client jar. An `.amalgam-install.json` pin in the instance prevents installers from ever re-running.
- **Forge bridges**: the launcher copies the matching maintained Forge bridge into the instance mods directory for 1.12.2, 1.18.2, 1.19.2, and 1.20.1. Module capability differences remain version-specific and are exposed by the support matrix.
- **Rules/args**: OS `arch` checks, `features` gating (never match in Amalgam), object/rule-gated JVM+game args; `rules_allow` follows upstream semantics — no rules → allow, rules present but none matching → disallow.
- **Quilt**: version-independent `quilt-loader` beta via meta.quiltmc.org; reuses the Fabric bridge jar (Quilt Loader runs `fabric.mod.json` mods natively) and downloads the plain `fabric-api.jar`.

### Launch pipeline (`launch.cpp`)

1. Resolve Java (`resolve_java_impl`: user override → cache → Adoptium ZIP → `javas/jdk-<major>`).
2. Resolve client jar (`vj.client.url`, else local patched jar, else `<instance>/<mc>.jar`; warn and continue if absent).
3. Verify/download every library into `libraries/` (mirrors JSON `downloads` paths; maven fallback for profile libs). Every download is checked against the metadata's **sha1 + size** before it is accepted; cached files are re-verified before reuse.
4. **Native jars** (`:natives-windows` classifier) are extracted to `<instance>/natives` via hardened `extract.cpp`, NOT added to the classpath.
5. Verify/download assets (object index + objects, sha1/size checked).
6. Copy the bridge mod to `<instance>/mods/amalgam.jar` (Fabric also fetches `fabric-api`).
7. Build the JVM command: `-agentpath:<dll> -Damalgam.dll.path=<dll>`, classpath via `join_classpath` (`;` separators), `-Djava.library.path=<natives>`, game args, and `--quickPlayMultiplayer` for quick play.
8. Spawn via `CreateProcessW`; update instance `last_played`; optionally wait for exit (`wait_for_exit`) and surface the exit code.

Instances live at `<base_dir>/instances/<name>/`, described by `instance.json` (MC version, loader + loader version, java path, memory, group, favorite, last_played).

### Network layer (`net.cpp`)

- **URL policy**: HTTPS only, no embedded credentials (`InternetCrackUrlW`); redirected destinations are revalidated before use.
- **Integrity**: SHA-1 via CryptoAPI; `verify_file(path, sha1, size)`; downloads go to `.part` and are atomically renamed on success.
- **Robustness**: 30 s connect / 60 s receive-send timeouts, 3 attempts, 64 MiB response cap for JSON APIs, 2 GiB download cap.

### Archive extraction (`extract.cpp`)

Zip magic check, `tar -tf` entry scan (rejects `..`, absolute paths, drive letters, ADS/`*?<>"|`/control chars), 20k-entry and 4 GiB caps, staging extraction, reparse-point rejection before final move.

## Utility client (amalgam.dll)

- `dllmain.cpp` — native DLL entry and exports are connected to the bridge; `bridge.cpp` owns `JNI_OnLoad`/`Agent_OnLoad` and render initialization.
- `core/protocol.h` — **V2 wire format** shared C++↔Java (byte-identical, enforced by tests):

  **Action** (36 bytes, little-endian): `u32 seq, u8 module_id, u8 kind, u8 flags, u8 pad, f32 x,y,z, f32 f, i32 target_id, i32 int_a, i32 int_b`.

  **Snapshot V2** (magic `"AML2"` + 60-byte header + up to 64 × 96-byte entities):
  - magic `0x324C4D41` at byte 0; floats `px,py,pz,pitch,yaw,vx,vy,vz,fall_distance,hurt_time`; i32 `selected_slot`; u32 `flags` (bit0 on_ground, bit1 breaking); i32 `hostile_count` (byte 52); i32 `entity_count` (byte 56); entities from byte 60.
  - Entity (96 bytes): `i32 id, i32 kind, f32 x,y,z, f32 dist_sq, f32 health, u32 flags, char name[32] (at +32), 4 pad, char team[24] (at +68), 4 pad`.
  - The V2 entity block supersedes the legacy hostile block — when `entity_count > 0` the parser forces `hostile_count = 0` (both blocks share the region after the header). Negative/oversized counts are clamped on both sides.

- `core/pipeline.{h,cpp}` — thread-safe FIFO action queue (`PIPELINE_CAPACITY = 1024`), cleared on shutdown, disconnect, and safe-mode activation, then drained at tick start.
- `core/tracker.{h,cpp}` — snapshot accumulation: skips invalid ids, prunes stale entries (default 3000 ms, `set_stale_timeout_ms`), stable-sorts by distance ascending.
- `core/game_mode_telemetry.{h,cpp}` — parses bounded `TLM1` sidebar frames, detects safe game-mode profiles, records match-relative markers, finalizes match summaries, aggregates session statistics, and exports local CSV reports. `F7` adds a manual marker; this path never enters `Pipeline`.
- `core/diagnostics_outbox.{h,cpp}` — explicit opt-in, local-only SQLite summary outbox using Windows `winsqlite3`; capped at 256 rows with export/delete controls. It does not upload data.
- `core/replay.{h,cpp}` — bounded binary telemetry replay recorder/player (`AMLRPL01`); records redacted state metrics, match-relative markers, and summaries only. It is not a Minecraft world replay and never records packets.
- `core/modules.cpp` — modules (`FREECAM=1, FLY=2, SPEED=3, NOFALL=4, AUTOTOOL=5, KILLAURA=6`); each `ModuleTickFn` reads a `Snapshot` + params and pushes `Action`s. Game-mode profiles (BedWars, SkyWars, UHC, Crystal PvP, SMP/Factions, and Practice) are implemented in the separate telemetry store and are HUD/telemetry only — zero new packets.
- `bridge/bridge.{h,cpp}` — JNI exports `Java_amalgam_bridge_NativeBridge_*` (attach, drain, clearActions, enterTick, exitTick, isEnabled, param, sendNow, setParam, shutdown).
- `render/` — ImGui menu + input capture over the game's GL context.

## Java bridge mod

- `NativeBridge.load()` reads `-Damalgam.dll.path`, loads the DLL, declares the native methods matching the DLL exports exactly.
- `Actions.java` mirrors the wire constants; V1/V2 snapshot parsing and action building are little-endian and covered by `ProtocolTest.java` (compiled and run with a real JDK from CTest).
- `AmalgamClient` (Fabric `ClientModInitializer`) attaches on init; `TickHandler` registers at `START_CLIENT_TICK`.
- Each tick (in `try/finally`): `nativeEnterTick` → sync controller state → `SnapshotBuilder.build()` (V2 with entities) → `nativeDrain(snapshot, actionBuf)` → parse + `ActionExecutor` → `nativeExitTick` in `finally`.
- Controllers translate actions to **vanilla packets only**:
  - `VelocityController` → `PlayerMoveC2SPacket` velocity + `setOnGround(false)` for fly.
  - `NoFallController` → `PlayerMoveC2SPacket.OnGroundOnly` (signature varies by version).
  - `AttackController` → `interactionManager.attackEntity` + `swingHand`.
  - `SlotController` → slot field vs `getSelectedSlot()/setSelectedSlot(int)` (per-version `Compat`).
  - `FreecamController` → static camera state via `CameraMixin` (`@Shadow setPos/setRotation`; `Camera.setup` signature varies by version).
  - `ActionExecutor` dispatches `SET_VELOCITY→Fly`, `SET_POS→Freecam`, `MOVE_ON_GROUND→NoFall`, `ATTACK→Killaura`, `SWAP_SLOT→Autotool`, `SELECT_SLOT`.

## Mods & AI

- Provider-specific live checks validate Modrinth public access and configured
  CurseForge access without downloading content.
- `readiness.{h,cpp}` provides player-focused local/live preflight for bridge
  files, Java runtimes, Microsoft account presence, provider access, and
  Bedrock availability.

- `mods.{h,cpp}` — Modrinth v2 (search, project, versions, dependency-resolved install) and CurseForge v1 clients; results carry `source` so the UI shows where each came from.
- `ai.{h,cpp}` — OpenAI-compatible client: `chat`, `vision` (base64 images), `image` (`/images/generations` → PNG; native `/api/generate` fallback for local Ollama). Multi-provider failover: `ai_providers[0]` is primary, the rest are tried in order on failure. Verified providers (Ollama Cloud, OpenRouter, Groq, Gemini) are documented in AGENTS.md.

## Testing

36 CTest checks are registered with `BUILD_TESTING` in `cpp/tests/`: focused native protocol, camera, tracker, telemetry, replay, diagnostics, module, mod, AI, archive/import, JSON/config/auth, model, network, performance, content, Bedrock, Java, UI model, Essentials, server-provider, readiness, and version-catalog suites; launcher `--check-java` and prerequisite smoke checks; and separate Fabric/NeoForge/Forge Java protocol/codec and server-transport checks.

## Security posture

No diagnostics persistence by default, no credentials in source, strict URL policy, hardened extraction, redacted summaries, explicit local export/delete, and no current remote upload. The client explicitly does NOT add anti-detection or stealth behavior; unsafe legacy action modules remain separate from the telemetry roadmap and are disabled by server-safe mode. See `docs/security.md` and `docs/diagnostics-api.md`.
