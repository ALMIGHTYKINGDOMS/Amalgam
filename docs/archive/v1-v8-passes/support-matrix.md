# Support Matrix

Status: **done** = built and wired into the launcher; **designed** = design exists, adapter not yet implemented.

## Loaders × Minecraft versions

| Loader | 1.18.2 | 1.19.2 | 1.20.1 | 1.21.1 | 1.21.4 | 1.21.5 | 1.21.6 | 1.21.8 | 1.21.11 |
|---|---|---|---|---|---|---|---|---|---|
| Fabric | launcher + bridge | launcher + bridge | launcher + bridge | launcher + bridge | launcher + bridge | launcher + bridge | launcher + bridge | launcher + bridge | launcher + bridge (pinned target) |
| Quilt | launcher + Fabric bridge | launcher + Fabric bridge | launcher + Fabric bridge | launcher + Fabric bridge | launcher + Fabric bridge | launcher + Fabric bridge | launcher + Fabric bridge | launcher + Fabric bridge | launcher + Fabric bridge |
| NeoForge | — | — | — | launcher + bridge | launcher + bridge | launcher + bridge | launcher + bridge | launcher + bridge | launcher + bridge |
| Forge | launcher + bridge | launcher + bridge | launcher + bridge | — | — | — | — | — | — |
| Vanilla | — | — | — | — | — | — | — | — | designed |
| Bedrock (UWP) | — | — | — | — | — | — | — | — | same launcher, separate adapter |

Notes:

- **Quilt** reuses the Fabric bridge jar (`quilt-loader` beta 0.20.0-beta.9 via meta.quiltmc.org; quilt-loader runs `fabric.mod.json` mods natively) + the plain `fabric-api.jar`. Launcher discovery starts at MC ≥ 1.14.4; bridge artifacts are currently built/tested from 1.18.2 upward.
- **Forge** covers 1.12.2 → 1.20.1 through the Forge installer route and the `java-forge-1.12.2`, `java-forge-legacy`, and `java-forge` bridges. NeoForge 1.20.1 is DELISTED on maven.neoforged.net — use Forge there.
- **Fabric 1.21.11** is the pinned utility-client target (no forward-compat commitment).

## Per-version Java bridge deltas

| Version | Slot accessor | NoFall packet | Camera.setup |
|---|---|---|---|
| ≤1.21.1 | public `selected` field | `OnGroundOnly(boolean)` | `(BlockGetter,...)` |
| 1.21.4 | public `selected` field | `OnGroundOnly(boolean,boolean)` | `(BlockGetter,...)` |
| ≥1.21.5 | `getSelectedSlot()/setSelectedSlot(int)` | `OnGroundOnly(boolean,boolean)` | `(BlockGetter,...)` |
| 1.21.11 | `getSelectedSlot()/setSelectedSlot(int)` | `OnGroundOnly(boolean,boolean)` | `(Level,...)` |

Wire format (`protocol.h` ↔ `Actions.java`) is version-independent and byte-identical across all bridges.

## Client modules

FREECAM=1, FLY=2, SPEED=3, NOFALL=4, AUTOTOOL=5, KILLAURA=6 — all applied via vanilla packets at `START_CLIENT_TICK`.

Game-mode profiles (scoreboard/objective parsing, BedWars/SkyWars/UHC/Crystal/SMP/Factions/Practice HUDs, auto-detection, timer/objective widgets, match summaries, markers, session statistics, and local exports): implemented in the native telemetry store and Fabric/NeoForge bridge collection path. All HUD/telemetry/tracking — zero new packets. A binary replay recorder/player and remote diagnostics upload remain separate work.

## Java runtimes

Launcher auto-provisions Adoptium JDKs per target (1.12.x → 8/11/17, modern → 21) and detects installed runtimes (`--check-java`). Per-instance `java_path` override supported.
