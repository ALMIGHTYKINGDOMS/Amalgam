# Amalgam Launcher + Client — Full Product Audit

Audit date: 2026-08-22

This report measures the product against the supplied Amalgam launcher and
in-game client reference boards. Percentages are weighted engineering
estimates, not counts of files, screens, or lines of code.

## Executive status

| Measure | Realistic status | Meaning |
|---|---:|---|
| Feature/code milestone | **91%** | The primary launcher, profile, content, Java, server, Bedrock, account, and in-game client paths exist and are connected. |
| Supplied launcher reference realization | **88%** | The visual hierarchy, routing, major page set, responsive shell, real catalogs, profile pages, server center, and account/settings surfaces are implemented. |
| Supplied in-game client reference realization | **84%** | The HUD, module browser, editor, profiles, cosmetics, performance, mod list, notifications, settings, diagnostics, and launcher handoff exist; final live-game validation remains. |
| Private beta readiness | **87%** | Suitable for controlled testing on the developer machine after Microsoft sign-in; Modrinth is live and the package prerequisites are present. |
| Public release readiness | **64%** | Signing, clean-machine evidence, production credentials, full live launch coverage, legal/support readiness, and soak testing remain. |
| Whole advertised product | **82%** | Includes unfinished external cloud/social services and all public-release obligations, not only local code. |

The product is no longer a prototype shell. It is a substantial private-beta
application. It is not honest to call it a finished AAAA public release until
the external and live-validation gates below are closed.

## Verification evidence

- The current standard runtime build and the isolated beta build both compile
  and link successfully.
- **35/35 automated tests pass** in the standard runtime build. Coverage
  includes protocol compatibility, module state, imports, transactional JSON,
  updates, credentials, networking, Java, Bedrock, provider parsing, cloud
  provider contracts, launcher smoke, Java bridges, real local-server process
  transport, and launcher-to-client profile bridging.
- **21/21 launcher routes render to PNG with exit code 0**: Home, Discover,
  Library, Profile, Project, Downloads, Settings, Account, Servers, Server
  Detail, Server Console, Bedrock, Essentials, Java, Backups, Logs, Config,
  Theme, Performance, Social, and Mods.
- Runtime prerequisites pass: archive support, native client DLL, Windows
  SQLite, and the MSVC runtime are present.
- The runtime package contains **19 Java bridge JARs** across Fabric,
  NeoForge, and Forge versions.
- The release candidate packages the recursive branding tree, including all 20
  generated art assets, the art manifest, the client HUD atlas, and the client
  animation-effects atlas. Package validation checks every manifest reference.
- The launcher and in-game client About/header surfaces use the real Amalgam
  logo, with a recovery-only fallback for incomplete developer directories.
- Latest local verification: **38 release-tooling tests**, **24 runtime-agent
  tests**, and **65/65 production schema checks** pass; the clean package and
  clean extracted-package runtime validators pass. Inno Setup is not installed
  on this workstation, so installer compilation remains a CI gate.
- Live provider check: Modrinth succeeds. CurseForge currently rejects the
  configured key with HTTP 403; this is a production credential gate.
- Minecraft Java launch readiness is correctly blocked until the user finishes
  Microsoft sign-in. Bedrock installation and add-on data are detected.

## What was made real in this pass

### Launcher

- Added safe `--page` deep links so the in-game client can open the correct
  Launcher page instead of showing non-functional toast actions.
- Fixed a command-line lifetime bug that could read freed argument memory while
  applying safe-mode/window options.
- Rebuilt the Servers experience around real local server state: Java process
  startup, stdin commands, stdout/stderr console capture, graceful shutdown,
  restart, EULA gate, local registry paths, and live status snapshots.
- Added integration coverage that starts a real probe process, sends a console
  command, observes output, and shuts it down.
- Upgraded Home, Account, Profile, Project, Bedrock, Server, and empty-state
  presentation based on the reference visual language.
- Removed stale profile fixtures and mock identity leakage from normal user
  state. Visual fixtures remain isolated to screenshot testing.
- Kept real Modrinth/CurseForge project art as the first choice and uses branded
  local artwork only as a fallback.
- Produced deterministic visual review images for every major route.

### In-game client

- Client state is now scoped to the **active Minecraft profile**, not the DLL
  installation folder.
- Launcher launch preparation writes a real `shared_profile.txt` containing the
  selected profile, Minecraft version, loader, loader version, mod count,
  instance directory, launcher version, and sync timestamp.
- Friends, servers, cosmetics, profile data, installed mods, screenshots, logs,
  and crash reports resolve from the selected instance.
- Added an integration test proving profile, mod, friend, server, and cosmetic
  data reach the client bridge.
- Profile controls now open the real Launcher Library.
- Mod-folder, logs, and crash-report actions open real profile paths.
- Crash diagnostics classify common memory, mixin, dependency, Java, and
  graphics failures from the latest report instead of always claiming an
  incompatible mod.
- Server overlay mock Disconnect/Rejoin actions were replaced with real Launcher
  navigation, Copy IP, and list refresh actions.
- Cosmetic equip state persists per profile with one equipped item per category.
- Notifications have a real global enable setting, placement setting, and test
  action instead of several controls writing the same boolean.
- Profile Sync now checks the actual server, friend, cosmetic, mod, settings,
  and keybind bridge files instead of displaying hardcoded success marks.
- The touched in-game client sources build without compiler warnings in the
  strict beta configuration.

### Original art

- Added a voxel-fantasy multiplayer settlement scene for server cards and server
  headers.
- Added a voxel-fantasy portal sanctuary scene for Account and identity surfaces.
- Both assets are original, text-free, and avoid copyrighted characters. Their
  source prompts and fallbacks are tracked in the art manifest.

## Reference-board coverage

### Launcher board

| Reference area | State | Notes |
|---|---|---|
| Home | Implemented | Branded hero, real account state, recent profiles, update/recommendation surfaces, responsive layout. |
| Discover | Implemented | Populated initial Modrinth catalog, unified filters/search, provider badges, real project routing; CurseForge awaits a valid key. |
| Library/profile manager | Implemented | Real profiles only in normal mode, responsive cards, play/manage actions, full profile page. |
| Downloads | Implemented | Persistent operation records, pause/cancel/recovery states, progress and history. |
| Project detail | Implemented | Info, content, changelog and version data with install workflow; availability varies by provider response. |
| Servers/console | Implemented locally | Real process transport and console; cloud host remains backend-dependent. |
| Settings/account | Implemented | Microsoft and Amalgam account surfaces, Java/performance/theme/translation settings and diagnostics. |
| Bedrock | Implemented baseline | Detect, launch, profiles and add-on import exist; a full live add-on matrix remains. |
| Essentials/social | Implemented baseline | Local/UI flows exist; reliable cross-user production behavior requires deployed backend services. |
| Installer/loading/crash surfaces | Implemented baseline | Packaging exists; signing, reputation and clean-machine certification remain. |

### Client board

| Reference area | State | Notes |
|---|---|---|
| HUD and HUD editor | Implemented | Modular HUD, presets, drag/edit controls, profile-local configuration. |
| Client menu/module browser | Implemented | Search/categories/settings and real module state. |
| Performance center | Implemented | Real client metrics and toggles; live cross-version profiling remains. |
| Mods and packs | Implemented | Reads the active instance rather than a demo array. |
| Profiles/sync | Implemented locally | Uses launcher-selected profile and bridge files; remote cloud sync remains backend-dependent. |
| Cosmetics | Implemented locally | Ownership bridge and persistent equip state; visible player-model rendering for every cosmetic is separate content work. |
| Notifications/settings | Implemented | Persisted settings and functional navigation/actions. |
| Diagnostics | Implemented | Real log/crash discovery and reason classification. |
| Friends/server overlays | Partial production | UI and launcher handoff exist; multiplayer presence/invites require live backend validation. |

## Remaining work, ordered by release impact

### P0 — production credentials and ownership validation

1. Replace or re-authorize the CurseForge API key; the current key returns
   HTTP 403. Re-run the live provider doctor until both catalogs report READY.
2. Complete Microsoft device sign-in with a Minecraft-owning account and verify
   the Xbox Live, XSTS, Minecraft entitlement, profile, and launch chain.
3. Confirm the Entra application is configured as a public desktop client for
   personal Microsoft accounts and that publisher/permission requirements are
   satisfied for distribution to other users.

Acceptance: a fresh user can search both providers, install a pack, sign in,
and launch an owned Java profile without developer intervention.

### P0 — live Minecraft matrix

Run install/launch/exit/relaunch tests for the maintained Fabric, NeoForge,
Forge, Quilt, vanilla, and Bedrock paths. Include clean profiles, modded
profiles, wrong-Java recovery, offline mode behavior, crash discovery, and
profile-local client state.

Acceptance: every advertised maintained bridge launches on a clean profile and
the client opens with the correct version, loader, mods, paths, and saved UI.

### P0 — server reliability

Run real Paper/Fabric server soak tests, console command tests, crash/restart,
backup creation, backup restore, world integrity checks, memory limit behavior,
and shutdown during an active save.

Acceptance: repeated start/stop/restart and restore cycles do not lose the
world, registry, console log, or EULA state.

### P1 — public desktop release

1. Purchase/use a trusted Windows code-signing certificate and sign the
   launcher, DLL, installer, and updater artifacts.
2. Test install, update, repair, uninstall, and rollback on clean Windows 10/11
   virtual machines at 100%, 125%, 150%, and 200% scaling.
3. Validate SmartScreen behavior, file associations, shortcuts, permissions,
   antivirus false positives, and installation without developer tools.
4. Finalize privacy policy, terms, third-party notices, Minecraft/Microsoft
   trademark language, support channel, data deletion, and incident response.

### P1 — accessibility, localization, and polish

- Complete keyboard-only traversal, focus visibility, screen-reader labels,
  reduced-motion behavior, and contrast checks.
- Finish translation coverage for launcher-owned strings and cache provider
  description translations with language/fallback labeling.
- Test long German/French text, CJK, right-to-left layouts, missing glyphs,
  minimum window size, ultrawide, and multi-monitor DPI transitions.
- Remove remaining compiler warnings in active code and split the oversized
  launcher UI unit into page-level modules to reduce regression risk.

### P2 — service-backed features

- Deploy and validate cross-user friends, parties, invites, presence, TURN
  relay, profile sync, entitlements, cloud backup, and cloud hosting.
- Add backend observability, rate limits, abuse controls, retention rules,
  migrations, disaster recovery, and a documented degraded/offline mode.
- Only advertise cloud-hosting or cross-device sync as generally available once
  the production service and support process exist.

## Recommended release sequence

1. **Internal alpha:** current code, Modrinth, local profiles, local servers,
   and controlled Microsoft account testing.
2. **Closed beta:** valid CurseForge credentials, signed candidate, full launch
   matrix, clean-machine installer evidence, server soak evidence.
3. **Public beta:** production auth/backend, crash/update telemetry with consent,
   legal/support pages, rollback drill, staged rollout.
4. **1.0:** all P0 and P1 gates closed, seven-day stability window, no critical
   crash/data-loss/security defects.

## Bottom line

The visual concepts are now represented by working launcher and client systems,
not static mockups. Local code quality and automated coverage are strong enough
for a controlled beta. The largest remaining gaps are live Minecraft/provider
validation and public-release operations—not another wholesale UI rewrite.
