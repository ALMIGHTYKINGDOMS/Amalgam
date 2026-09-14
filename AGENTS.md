# AGENTS.md

## Project

Amalgam: C++ utility-client DLL + ImGui launcher + Java bridge mod for Minecraft
1.12 → latest, unified in ONE launcher (no separate launchers per edition/loader).
Java bridges: Fabric 1.18.2 / 1.19.2 / 1.20.1 / 1.21.1 / 1.21.4 / 1.21.5 / 1.21.6 /
1.21.8 / 1.21.11 (Yarn, `java/`), NeoForge 1.21.1 / 1.21.4 / 1.21.5 / 1.21.6 /
1.21.8 / 1.21.11 (Mojang, `java-neoforge/`); Forge 1.20.1 has a maintained bridge
in `java-forge`, while older Forge versions remain launcher/menu-only when no
maintained bridge jar is available
(1.18.2–1.20.1). Quilt is launch-supported from the same launcher: it uses the
version-independent `quilt-loader` beta (0.20.0-beta.9) via meta.quiltmc.org,
reuses the fabric bridge jar (Quilt Loader runs `fabric.mod.json` mods natively),
and downloads the plain `fabric-api.jar` (Quilted Fabric API only covers ≤1.20.1).
Quilt loader covers MC ≥1.14.4. Bedrock (UWP) is supported from the same launcher:
detect / launch / install addons.

### Java bridge build (NeoForge)

Multi-project build in `java-neoforge/` (Gradle 9.5.0, JDK 21, NeoForgeGradle
userdev 7.1.38). Version-independent classes (package `amalgam.neoforge`:
ActionExecutor, TickHandler, SnapshotBuilder, Velocity/Freecam/Attack/Slot
controllers, Amalgam entry) live in `java-neoforge/common`; per-version deltas
live per-subproject: `Compat.java` (slot accessor + onGround — 1.21.5+ uses
`getSelectedSlot()/setSelectedSlot(int)` + `onGround()`; ≤1.21.4 uses the public
`selected` field), `NoFallController.java` (`StatusOnly(boolean)` ≤1.21.1,
`StatusOnly(boolean,boolean)` ≥1.21.4), `mixin/CameraMixin.java`
(`Camera.setup(BlockGetter,...)` ≤1.21.10, `(Level,...)` 1.21.11), resources
(`neoforge.mods.toml` + `amalgam.mixins.json`). NeoForge 1.20.1 is DELISTED on
maven.neoforged.net → Forge covers 1.20.1.

## Build

### C++ (launcher + DLL) — MSVC, ninja

```
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul 2>&1 && cmake --build cpp\build
```

Outputs: `cpp/build/amalgam_launcher.exe`, `cpp/build/amalgam.dll`.

### Java bridge — Gradle 9.5.0 + JDK 21 (Loom 1.17.19 REQUIRES Gradle 9.5.0)

Multi-project build: root `java/` (settings.gradle + shared build.gradle via
`subprojects {}`). Version-independent bridge classes live in `java/common`
(`amalgam.bridge`, plus per-version controllers are copied into each subproject).
Per-subproject version params are in each `gradle.properties`
(minecraft_version / yarn_mappings / fabric_loader / fabric_api / java_release).

```
$env:JAVA_HOME = "C:\Users\David\AppData\Local\Temp\opencode\jdk-21.0.12+8"
$env:GRADLE_USER_HOME = "C:\Users\David\AppData\Local\Temp\opencode\gradle-home"
C:\Users\David\AppData\Local\Temp\opencode\gradle-9.5.0\bin\gradle.bat -p java build
```

- The bash tool's `workdir` parameter does NOT apply to this project — always pass
  `-p <project dir>` (or call the exe from the build dir).
- Single subproject: `gradle.bat -p java :fabric-1.21.11:build`.
- After building, copy all jars to the launcher's bridges dir:
  `copy java\*\build\libs\amalgam-fabric-*.jar cpp\build\bridges\`
  (exclude `-sources.jar`).
- NeoForge build (separate tree, `java-neoforge/`): same gradle invocation with
  `-p java-neoforge build`; copy `java-neoforge\neoforge-*\build\libs\amalgam-neoforge-*.jar`
  to `cpp\build\bridges\`. NeoForge metadata prefix drops the MC leading "1."
  (1.21.11 → `21.11.`); versions with no stable (1.21.6) fall back to latest beta.
- Version-specific API deltas handled per subproject: `selectedSlot` field
  (≤1.21.4) vs `getSelectedSlot()/setSelectedSlot()` (≥1.21.5);
   `PlayerMoveC2SPacket.OnGroundOnly` single-arg (≤1.21.1) vs two-arg (≥1.21.4).
- Forge 1.20.1 bridge is a separate `java-forge/` ForgeGradle 6.0.53 project using
  Gradle 8.8, JDK 17, and Forge 1.20.1-47.4.15. Build with
  `gradle-8.8\bin\gradle.bat -p java-forge build`; copy
  `java-forge\build\libs\amalgam-forge-1.20.1.jar` to `cpp\build\bridges\`.
- Forge 1.18.2/1.19.2 bridges are built in `java-forge-legacy/` with ForgeGradle 5.1,
  Gradle 7.6.4, and JDK 17; copy `forge-*\build\libs\amalgam-forge-1.*.jar` to
  `cpp\build\bridges\`. Forge 1.12.2 is built in `java-forge-1.12.2/` with
  ForgeGradle 2.3.4, Gradle 4.10.3, and JDK 8.

## CLI (amalgam_launcher.exe)

```
--versions [loader]
--check-java
--java-install <8|11|17|21|25>
--check-official-launcher
--check-prereqs
--inspect <mc_id> [loader]
--launch <mc_id> [loader] [--dry-run]
--mods-search <query> [loader] [game_version] [facet]
--mods-install <slug> [loader] [game_version] [mods_dir]
--ai-chat <message> [model]
--ai-image <prompt> [out.png]
--ai-vision <image> [model]
--bedrock-info
--bedrock-launch
--bedrock-install <addon.mcpack>
--login
--logout
--doctor
--ui-snapshot
```

## Config (cpp/build/launcher.json)

- `ai_base_url` + `ai_api_key` are the legacy single-provider fields. Multi-provider
  failover uses the `ai_providers` array in `launcher.json`: index 0 = main, the rest
  are automatic backups tried in order on failure (chat, vision, and image all
  fail over). Each entry: `name`, `base_url`, `api_key`, `chat_model`, `image_model`
  (empty = no image support). Legacy fields fall back to a single "primary" provider
  when `ai_providers` is absent. Verified providers:

| Provider | base_url | chat | vision | image |
|---|---|---|---|---|
| Ollama Cloud | `https://ollama.com/v1` | `gpt-oss:20b`, `gpt-oss:120b`, `nemotron-3-nano:30b` | `gemma4:31b` | none on cloud |
| OpenRouter | `https://openrouter.ai/api/v1` | `nvidia/nemotron-3-ultra-550b-a55b:free` (1M ctx), `nvidia/nemotron-3-super-120b-a12b:free`, `openai/gpt-oss-20b:free` | `google/gemma-4-26b-a4b-it:free`, `nvidia/nemotron-nano-12b-v2-vl:free` | no free image models |
| Groq | `https://api.groq.com/openai/v1` | `llama-3.3-70b-versatile`, `openai/gpt-oss-20b`, `qwen/qwen3.6-27b`, `llama-3.1-8b-instant` | none | none |
| Gemini | `https://generativelanguage.googleapis.com/v1beta/openai` | `gemini-flash-latest`, `models/gemini-3.5-flash`, `models/gemini-3.1-flash-lite` | same as chat (uses `{url:...}` struct) | `gemini-2.5-flash-image` (free tier quota-limited, limit 0) |

- `deepseek-v4-flash` / `kimi-*` / `glm-*` / `minimax-*` / `qwen3.5` need paid subscription on Ollama Cloud + OpenRouter (403/429).
- Gemini model names: `gemini-flash-latest` (no prefix) and `models/gemini-3.x-*` (with prefix) both work; `models/gemini-2.5-flash` alone 404s.
- Gemini image input REQUIRES `image_url` as `{url:"data:...;base64,.."}` object — a plain string errors "not a struct" (our ai.cpp already sends the struct).
- Image gen fallback: Ollama has NO `/v1/images/generations`; `ai::image` falls back to native `/api/generate` (data-URI base64 in `response`) only when the OpenAI-style endpoint 404s — needs a local Ollama with `x/flux2-klein:4b` / `x/z-image-turbo`.
- Modrinth API needs NO token (public). CurseForge needs `curseforge_key` (set).
- `launcher.json` must be written WITHOUT UTF-8 BOM (the JSON parser rejects it). PowerShell 5.1 `Set-Content -Encoding UTF8` adds a BOM → use `[IO.File]::WriteAllText(path, text, New-Object System.Text.UTF8Encoding($false))`.

## Conventions

- **No custom packets.** All actions go through vanilla packets at tick start
  (`START_CLIENT_TICK`). See `docs/packet-whitelist.md`.
- Wire format (C++ `core/protocol.h` ↔ Java `Actions.java`) must stay byte-identical:
  - Action = 36 bytes little-endian.
  - Snapshot V1 = 52-byte header + ≤32 × 8-byte hostiles.
  - Snapshot V2 = `AML2` magic + 60-byte header + ≤64 × 96-byte entities; entity data
    supersedes the legacy hostile region when `entity_count > 0`.
- Read-only sidebar telemetry uses a separate bounded `TLM1` JNI payload (≤15 lines);
  it never enters the action queue or changes the packet whitelist. C++ parses mode
  profiles, match markers, and session summaries; the HUD renders/exports them. An
  explicitly enabled Windows-only SQLite outbox stores aggregate summaries locally;
  no remote upload or automated game-mode actions are permitted.
- Module ids: FREECAM=1 FLY=2 SPEED=3 NOFALL=4 AUTOTOOL=5 KILLAURA=6.
- Action kinds: SET_VELOCITY=1 SET_POS=2 MOVE_ON_GROUND=3 ATTACK=4 SWAP_SLOT=5 SELECT_SLOT=6.
- Paths: JSON lib paths use `/`; convert to `\` for filesystem (`native_rel`), back to
  `/` for URLs (`rel_url`). `net::mkdirs` splits on `\` only.
- Launcher → game: `-agentpath:<dll> -Damalgam.dll.path=<dll>`; bridge mod copied to
  `<instance>/mods/amalgam.jar`.
- Native libs = separate library entries with `:natives-<os>` in the name → extract to
  `<instance>/natives`, NOT classpath.
- Java compile errors: check signatures against the Loom merged jar via
  `javap` (e.g. `Camera.setPos/setRotation` are `protected` → `@Shadow`).
- Secrets (Modrinth token, CurseForge key, AI key) live in `launcher.json` next to the
  exe, never in code or git.

## Notes

- Windows shell is PowerShell 5.1: use `;` / `if ($?) {}` for chaining (no `&&`).
- Temporary toolchain under `C:\Users\David\AppData\Local\Temp\opencode\`.
- Extractor = `C:\Windows\System32\tar.exe -xf <archive> -C <out>`.
