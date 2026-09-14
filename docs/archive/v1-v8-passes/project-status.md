# Project Status

Status reviewed: 2026-08-22.

## Official Release Candidate Pass — 2026-08-22

The exact `3.0.0-beta.1` ZIP and branded Inno Setup installer now build,
validate, install, and start successfully. Final verification is **35/35 native
tests, 38/38 release/security tests, 24/24 runtime-agent tests, 65/65 schema
checks, 19/19 bridge jars, and 22/22 visual routes**. An eight-route 1024x720
pass also confirms responsive reflow. The final installed smoke copy detects
Java and Bedrock, passes Windows prerequisites, returns live Modrinth results,
creates a protected first-run config, and opens with an empty real library.

The realistic current estimate is **87% for the whole advertised product, 97%
for the feature/code milestone, 94% for private beta readiness, and 82% for
official public launch readiness**. The public launch score remains lower
because the live CurseForge proxy still needs an authenticated provider check,
all Windows artifacts are unsigned, and Microsoft/Minecraft owner login plus
real in-game client validation require a human account that owns Minecraft. Full
evidence and exact hashes are in
`docs/official-launch-readiness-2026-08-22.md`.

## Launcher + Client Realization Pass — 2026-08-22

The launcher and in-game client reference boards have been converted into real,
profile-aware product flows. The standard runtime build and the isolated beta
build both compile, **35/35 tests pass**, and all **22 visual-review routes**
render successfully. The release build contains 19 Java bridge JARs; Windows
prerequisites, Modrinth, Java, the native client DLL, and Bedrock detection pass
the live readiness check.

The in-game client now reads the active instance rather than the DLL folder,
receives real profile/friend/server/cosmetic data from the launcher, scans the
selected profile's mods, opens real launcher pages and profile paths, persists
cosmetic selections, and reports actual crash evidence. Local server controls
now use real Java process stdin/stdout transport and graceful stop semantics.

The current weighted estimate is **87% for the whole advertised product, 97%
for the feature/code milestone, 94% for private beta readiness, and 82% for
public release readiness**. The full current evidence is in
`docs/official-launch-readiness-2026-08-22.md`; the earlier reference-board
mapping remains in `docs/full-product-audit-2026-08-22.md`.

The release candidate now packages the complete recursive branding tree,
including the generated client HUD/animation atlases. The package validator
checks the art manifest and every referenced runtime asset, and the clean
runtime smoke test verifies the client-loading, client-menu, HUD, animation,
Servers, Essentials, and Performance art. The in-game client and launcher About
surfaces also use the real Amalgam logo with a recovery-only fallback.

### Art realization pass — 2026-08-22

The original Minecraft-inspired art package now includes launcher Discover,
modpack, onboarding, six loader/profile covers, Servers, Essentials,
Performance, and account/server scenes. The in-game client now has a menu
header, diagnostics scene, splash/loading scene, and transparent 4x4 HUD and
animation-effects atlases. Launcher loading overlays use the loading scene behind the real
progress card; client menus load the optional textures through WIC/OpenGL and
fall back cleanly when a minimal package omits them. Home, Servers, Essentials,
and Performance visual captures were rendered against the rebuilt runtime.

Current external/live gates: all 24 protected Supabase Edge Functions are now
deployed to the correct Amalgam project and anonymous security-definer RPC
access was removed. Remaining gates are the authenticated CurseForge provider
check, Microsoft sign-in with an owning account, public signing, clean-machine
game coverage, server/Bedrock soak tests, and legal/support readiness.

Latest local verification after the release-hardening pass: **35/35 native
CTest suites pass, 38 release-tooling tests pass, 24 runtime-agent tests pass,
65/65 production schema checks pass, and the final clean package plus runtime
validators pass**. Inno Setup 7.1.0 is installed; the final branded installer
compiled and passed an isolated install/runtime/GUI smoke test. Public release
still requires Authenticode signing.

## Build Recovery & Completion Pass — 2026-08-21

This pass restored a green native build after the V3 client UI work had left
`client_ui.cpp` and `social_ui.cpp` non-compiling (the prior "all balanced"
status checks did not exercise the compiler).

Fixed in this pass:

- `client_ui.cpp`: enabled `IMGUI_DEFINE_MATH_OPERATORS` (ImVec2 +/- operators),
  moved the `draw_radial_menu` forward declaration into `aml::client`,
  declared the previously-orphaned `render_mods_page`/`render_essentials_page`/
  `render_screenshots_page` methods, corrected the unscoped `ModuleId` enum
  usage (`MOD_FREECAM`/`MOD_FLY`/… instead of `ModuleId::FREECAM`), bound the
  by-value `tracker::store().entities()` result to a local (not `auto&`), added
  missing `<shellapi.h>`/`<fstream>` includes, fixed an out-of-range `\xa7a`
  escape, and wrapped every settings `switch` case in braces to resolve C2360
  jump-over-initialization errors.
- `social_ui.cpp`: repaired the `shared_servers.txt` writer to use the real
  `config::Server` fields (`name`, `address`; no `port`/`is_online`).
- Wired the three orphaned client pages into a reachable sub-page mechanism
  (Mod & Packs, Essentials Overlay, Screenshots) from the dashboard quick
  actions, with Back navigation and correct tab/keyboard reset.
- Made the shared launcher↔client server bridge consistent: the launcher now
  writes `name|address` and the client parses that same pipe format, with a
  `server_endpoint()` helper so display/Copy-IP no longer fabricate a port.
- Removed dead `draw_draggable_hud_element` helper and wired the HUD editor's
  "Quick Presets" buttons to real module toggles (`module_set_enabled` /
  `modules_reset_defaults`) instead of empty placeholder bodies.
- Removed the in-game cosmetics page's hardcoded demo item array. Cosmetics now
  load from a `shared_cosmetics.txt` bridge written by the launcher, with
  ownership derived from the real entitlement plan (`is_plus()`: premium
  cosmetics require Amalgam+, free/common items unlock for everyone). The page
  shows a real empty state when no cosmetics are owned instead of fake content.

Verified after the pass:

- Native build links `amalgam_launcher.exe` and `amalgam.dll` cleanly.
- 31/31 CTest suites pass.
- `--check-prereqs` and `--doctor` run successfully; the only Java action item
  is the owner-performed Microsoft account connection (an external gate).

Remaining is unchanged from `docs/release-gates.md` and `docs/v3-remaining-work.md`:
code signing, clean-machine launch matrix, Bedrock UWP live validation,
CurseForge production approval, legal/support review, plus the non-blocking
`ui.cpp` split and centralized operation-manager follow-ups.

## Latest Audit - 2026-08-20

- Native build is green again: the launcher and client DLL link, and all 30
  CTest suites pass (28 original + `amalgam_essentials_test` +
  `amalgam_server_providers_test`).
- Server runtime provisioning is wired into Create Server: Paper, Purpur,
  Folia, and Fabric runtimes are downloaded from `fill.papermc.io`,
  `api.purpurmc.org`, and `meta.fabricmc.net`, SHA-256-verified, and installed
  as `server.jar`. Vanilla/Spigot/Forge/NeoForge/Quilt remain manual installs.
- The server-provider layer now covers Paper, Purpur, Folia, Velocity, Fabric
  (game/loader/installer), and Quilt metadata, with pure URL/parser functions
  unit-tested in `amalgam_server_providers_test`.
- The Servers page is unified into Host + Connect; the orphaned `aml::server`
  model and the dead external-address-book page were removed.
- Servers V3 restructures the page into **LOCAL | AMALGAM CLOUD**. Local server
  cards open a full detail panel (Overview, Console, Files, Players, Plugins,
  Properties, World). Cloud adds a provider abstraction (`IHostingProvider`),
  capability flags, remotely-configurable plan cards, a 6-step deployment
  wizard, a cloud dashboard mirroring the local UI, and a development-mode
  provider that clearly shows "CLOUD PROVIDER NOT CONNECTED" and never fakes
  deployments. External direct-connect servers moved to a Saved Servers
  section under Local.
- The account page gained a Membership card driven by a capability-based
  entitlement model (`AmalgamEntitlements`) fetched from the backend with
  interval caching: plan badge, TURN relay usage meter with limit warnings,
  cloud storage/server usage, and website links for upgrade/billing.
- New unit test `amalgam_cloud_hosting_test` covers the dev provider contract
  (plans, regions, software, no fake servers) and the entitlement model; the
  suite is now 31 CTest suites.
- Essentials received an AAA visual upgrade: a hero header with purple glow,
  stat counters (friends / invites / active sessions), and primary actions
  (Host World, Invite Friends, Join via Code). The Friends tab is now a
  premium 3-column control center (friends list | activity/session/invites |
  status/quick actions/connection/relay) with compact friend rows, an
  online/offline grouped list, a Recent Activity feed built from real
  notifications, a Quick Actions grid, a Your Status card with presence
  editing, a Connection card, an intentional no-friends empty state with
  feature cards, and a Friend Profile side panel (previously dead
  `selected_friend_id`). All rows and actions use real manager data; the
  visual-review fixture seeds demo friends/notifications exactly like the
  existing instance/mod/server fixtures.

## Latest Audit - 2026-08-18

- Managed Temurin Java runtime support is implemented for Java 8, 11, 17, 21,
  and 25. Downloads use the Adoptium assets API, verify size and SHA-256,
  extract with archive safety limits, write `runtime.json`, and validate the
  exact `java.exe` before activation.
- The V3 component pass is integrated into the live launcher pages. Deterministic
  OpenGL fixture captures for all 20 supported routes complete with zero ImGui
  stack errors, and the shared card/button contracts are balanced across child
  windows.
- The Java Manager installs runtimes asynchronously, supports rescan/open/set
  default/remove actions, and launch resolution prefers managed Java before a
  compatible system installation.
- A real Java 21 download/install completed locally, followed by a Fabric 1.21.1
  dry run using the managed executable.
- Native build and package validation remain green: 36/36 CTest suites pass,
  Fabric, NeoForge, Forge, legacy Forge, and Forge 1.12.2 builds complete, and
  the extracted package runtime checks pass.

## Completion Audit - 2026-08-16 (updated)

This pass closes the internally testable implementation work for the current
release candidate. The remaining items are external release gates, listed in
`docs/release-gates.md`; they are not being counted as finished merely because
the code builds.

- A clean extracted-package runtime smoke test (`tools/validate-package-runtime.ps1`)
  now validates each packaged candidate from a fresh extraction: Java
  detection, prerequisite checks, and a secret scan.
- Archive update-diff coverage now exercises the CurseForge manifest path
  (provider filenames flagged as staging-resolved) and the replaced/unchanged
  classification alongside the existing added/removed Modrinth case, in
  `import_pack_test`.

- Credentials now migrate to Windows DPAPI on save, and inaccessible protected
  values cannot be silently replaced.
- Provider installs stage and verify files before activation, reuse validated
  staging after interruption, back up replacements, and restore the prior
  profile when activation or metadata persistence fails.
- Downloads have real pause/cancel checkpoints, safer shutdown behavior, and
  restart-recovery records rather than a cosmetic paused state.
- Profile-changing work now enters one durable protected queue. Installs,
  imports, profile updates, creator-pack updates, and AI pack assembly cannot
  overlap their writes; Downloads shows queued state and queue position, and a
  restart still converts unfinished work into an explicit retryable recovery
  record.
- Each operation now retains a bounded, persistent step timeline plus elapsed
  and completion time in Downloads. The same safe transfer pause/cancel
  checkpoints are used by standard installs, performance optimization, and AI
  modpack assembly.
- The launcher embeds a Per-Monitor-V2 DPI manifest and renders at the physical
  display surface, addressing the bitmap-scaled/blurry UI failure seen during
  the audit.
- Branding uses the supplied logo and banner assets with deliberate contain and
  cover fitting. Vector favorites replace missing font-glyph stars.
- Original Minecraft-inspired voxel art now provides the Discover backdrop and
  labeled fallback covers for local/custom profiles. Real provider artwork
  remains the first choice whenever it is available.
- Deterministic, non-persistent UI fixtures now produce direct OpenGL PNG
  captures for Home, minimum-size Home, Discover, Library, profile detail,
  Downloads, and Settings. The capture review caught and fixed an auto-height
  card bug that had hidden profile tabs and content.
- All 36 CTest suites pass after the final renderer, DPI, module, projection,
  UI-component, and archive-diff changes.
- The Create Profile wizard has been upgraded: step indicator uses drawn circles
  with connecting lines, quick-start presets are clickable bordered cards,
  content source uses large labelled cards, project auto-detect resolves
  loaders/versions in the background, and inline validation errors appear for
  required fields.
- The shell window is now resizable via Win32 edge drag zones; window position,
  size, and maximized state are saved to `launcher.json` on close and restored
  on next launch.
- The status-bar `##log` child uses `Dummy()` to grow content bounds, fixing
  the ImGui debug warning about `SetCursorPos` extending boundaries.
- All 103 raw-pixel button sizes across every tab are now wrapped in `ui_px()`
  for consistent DPI scaling. Legacy home tab Dummy calls and stat cards also
  use `ui_px()`.
- Server removal now shows a confirmation modal before deleting, matching the
  existing pack-update confirmation pattern.
- Library search is now case-insensitive, matching user expectation.
- Card rounding in `card_begin()` is DPI-aware via `ui_px(12.0f)`.
- Browse "Load more results" button uses `ui_px()`.
- Instance detail hero banner height uses `ui_px()`.
- "Launch server" button renamed to "Join server" for clarity.
- Create Profile wizard: all 14 raw-pixel sizes converted to `ui_px()` for DPI
  scaling; review step now shows AI prompt, archive path, and Java override;
  instance creation failure pushes an error notice instead of silently logging.
- Servers and Modpack AI tabs show empty-state messages when no content is present.
- Redundant `PushStyleColor(ImGuiCol_Separator, ...)` wrappers removed from
  wizard separators (the global theme already sets this color).
-   Provider editor row and 5 instance-detail input widths use `ui_px()` instead
  of raw pixel values.
- Unit tests added for `extract.cpp` (archive extraction, process execution,
  path utilities), `auth.cpp` (client ID validation, account save/load round-trip,
  DPAPI protection), and `java.cpp` (version parsing, installed JDK scan).
  Test suite expanded from 25 to 28 CTest suites.
- Hardcoded colors in home create card gradient, discover hero background,
  version list text, and wizard error text replaced with theme constants
  (`k.bg`, `k.sidebar`, `k.surface`, `k.muted`, `k.red`).
- AGENTS.md CLI section updated with 5 missing commands (`--check-prereqs`,
  `--login`, `--logout`, `--doctor`, `--ui-snapshot`).
- CMake: ASAN (`-DAMALGAM_ENABLE_ASAN=ON`) and clang-tidy
  (`-DAMALGAM_ENABLE_CLANG_TIDY=ON`) options added; `cmake --install`
  rules added for launcher, DLL, and branding assets.

## Current Realistic Estimate

**Whole advertised project: approximately 80% complete, with a reasonable range
of 76-84%.** This is a weighted engineering estimate, not a percentage of
files or lines of code.

**Planned feature/code milestone: approximately 96% complete.** The native
protocol, telemetry, local diagnostics, replay, launcher baseline, account
flow, provider installs, bridge source, transactional recovery flows,
DPI/rendering work, tests, and package tooling are substantially implemented.

**Private developer beta: approximately 83%.** The launcher is usable with real
provider data, isolated profiles, Microsoft device-code sign-in, local recovery,
and validated artifacts, subject to the account owner and a supported
Windows/Minecraft installation.

**Public release readiness: approximately 60%.** The remaining gap is primarily
owned-account validation, clean-room runtime coverage, Bedrock validation,
signing/installer/bootstrap evidence, and platform/legal review rather than
missing primary UI screens.

| Area | Status | Notes |
|---|---:|---|
| Native protocol, HUD, telemetry, replay, local SQLite | 93% | V1/V2 tests, action contracts, camera projection, TLM1, markers, summaries, session stats, replay reader/writer, opt-in outbox |
| Fabric bridges | 84% | 9 projects build; broad clean-room game validation remains |
| NeoForge bridges | 83% | 6 projects build; camera-pose/projection path and V2 parser coverage added; runtime game validation remains |
| Launcher pipeline and UI | 97% | Reference shell, physical-pixel DPI rendering, supplied branding, adaptive library, player-facing status, account UI, recovery-first actions, transactional installs, archive change review, and deterministic visual QA |
| Quilt/Forge/Bedrock | 86% | Forge bridges cover 1.12.2 through 1.20.1 with module, camera, telemetry, and projection parity; cached dry-runs pass, while clean Bedrock coverage remains |
| Mods and AI | 94% | Hash-aware installs and export, ownership metadata, archive-reviewed staged/recoverable provider installs, published-pack updates, AI handoff, and live provider coverage |
| Packaging/release | 80% | Credential-free archive, SHA-256 component manifest, SBOM, prerequisites, and CI workflow; signing/bootstrap and external launch evidence remain |

The historical estimate below is retained only as an audit record and is
superseded by this section.

## Scope Decision

The full Amalgam utility client remains in scope for the product release. This
includes the native injection path and the advanced utility modules (including
Freecam, Fly, Speed, NoFall, AutoTool, and KillAura). They are not being
removed or silently disabled to pursue platform compatibility.

This means “CurseForge quality” is being used as the engineering and release
quality bar: reliable installation, clear support boundaries, polished UI,
clean recovery, signed artifacts, and reproducible validation. CurseForge
approval for the complete feature set remains a separate platform-review risk
and is not implied by this quality target.

## Latest Validated Slice

- Navigation icons are vector-drawn instead of relying on font glyphs, avoiding
  missing-icon boxes and improving DPI consistency.
- Primary and secondary buttons now grow to fit their scaled labels instead of
  clipping long actions such as “Create Custom Profile”.
- The launcher keeps its Win32 layout in one normalized coordinate space on
  scaled desktops, preventing oversized sidebars and clipped cards.
- The shell now uses an integrated borderless titlebar with a branded icon,
  working minimize/maximize/close controls, a full-height sidebar, and a
  right-aligned search/action cluster.
- Home and Library now follow the supplied dark navy/violet reference layouts:
  adaptive recent-profile cards, an always-available real create/import card,
  My Worlds and Collections views, and real local profile, recent-play,
  favorite, download, and server totals instead of fabricated activity data.
- Profiles without upstream project artwork now receive deterministic local
  instance canvases, so unbranded or custom profiles remain distinct and
  polished without being misrepresented as downloaded cover art.
- The Screenshot library is now an adaptive local thumbnail gallery: captures
  are shown newest-first with image preview/open actions, file-size context,
  an explicit folder action, and a 48-image rendering bound to keep large
  libraries responsive.
- The My Worlds library is now an adaptive thumbnail gallery using each world's
  real `icon.png` when present, with a deliberate profile-art fallback, direct
  open actions, and a route into the matching profile's World controls.
- Published creator-pack updates now recheck the latest compatible provider
  archive, show its archive details and changelog, create an updated copy for
  modified profiles, stage the full archive before commit, preserve worlds and
  runtime-owned data, and create a restore point before changing pack content.
- Profile content, screenshots, worlds, and whole profiles now use
  recovery-first actions instead of permanent deletion. World backups live
  outside Minecraft's `saves` folder, so they cannot be mistaken for playable
  worlds.
- JSON parse/write error outputs are now reset on every operation and covered
  by regression tests, preventing an earlier recoverable failure from making a
  later valid settings or metadata read appear to fail.
- The command-line installer now accepts the same explicit `modrinth` or
  `curseforge` source choice as the UI while retaining the old Modrinth-first
  syntax. Live isolated installs from both providers verified downloaded JARs
  and persisted provider/version ownership metadata.
- A real Fabric 1.21.1 no-account dry-run completed loader resolution, Java 21
  selection, asset verification, native extraction, bridge copy, and Fabric
  API verification without spawning Minecraft.
- AI modpack plans now keep provider and loader as separate fields through
  preview, install, profile-wizard creation, and manifest export. This fixes a
  path where an AI suggestion could accidentally use a provider value as its
  loader.
- Settings now has a player-facing live readiness check, also available through
  `--doctor [--online]`. It checks bridge files, Java runtimes, stored account
  presence, both provider catalogs, and Bedrock availability without exposing
  credentials or attempting a Microsoft sign-in automatically.
- A compact player-facing Launcher Status footer now replaces the default
  developer console; the same bounded local diagnostics remain available from
  the footer or `Ctrl + Shift + L` for troubleshooting.
- Settings has a navigable category rail; Java Manager can select and persist a
  per-major default runtime; Servers use actionable profile rows; and the
  existing install, launch, retry, recovery, Bedrock, AI, and native utility
  workflows remain wired behind the refreshed surfaces.
- The Home grid reserves its final gutter so its rightmost card stays visible,
  and the Settings category rail now uses the same custom scale-safe navigation
  treatment as the primary sidebar.
- Reference-size QA samples cover Home, Discover, Library, Settings, and
  Downloads; the status/diagnostics switch and Downloads navigation were
  exercised through the live UI.
- The main content region now uses the available viewport instead of leaving a
  fixed-width dead area on wide monitors.
- The My Modpacks toolbar now moves search, sort, and group filters to a
  dedicated row at narrow widths instead of competing with primary actions.
- Install, profile, group, and modpack-wizard dialogs now clamp to the usable
  viewport, and the Alerts/Downloads panels size and position themselves from
  the current work area instead of fixed desktop coordinates.
- Interrupted persisted operations now become retryable failures with a
  startup recovery notice that links directly to Downloads.
- Download/job state now replaces its JSON snapshot atomically, reducing the
  chance of a partially written recovery file after a crash or forced close.
- Protected profile operations now execute in a single visible queue. This
  prevents two independent install/update/import actions from modifying a
  profile at once while preserving safe cancellation and persisted recovery.
- Profile-content restore now stages the requested snapshot and creates a
  safety restore point before changing live data. This makes Restore reversible
  and covers managed content, launcher metadata, and game options while
  deliberately leaving Minecraft worlds untouched.
- The Settings category routes that were previously thin now expose real
  launcher defaults, download state, performance recommendations, alert
  controls, privacy/data locations, and advanced-mode controls.
- The isolated verification build compiles and all 30 native/bridge CTest suites
  pass after these UI and restore changes. A fresh release archive also passes
  its isolated package validation with all 19 runtime bridge jars.

## Historical Estimate Archive (Superseded - do not use for planning)

**Whole advertised project: approximately 73% complete, with a reasonable range of 68–77%.** This is a weighted engineering estimate, not a percentage of files or lines of code.

**Planned feature/code milestone: approximately 94% complete.** The native protocol, telemetry, local diagnostics, replay, launcher baseline, account flow, Forge 1.20.1 bridge, bridge source, tests, staged creator-pack update/recovery flows, and packaging work are substantially implemented. The whole-project number is lower because runtime validation and release operations are not equivalent to compiling code.

**Private developer beta: approximately 77%.** The native protocol, HUD telemetry, local diagnostics, account flow, Forge 1.20.1 bridge, cached loader dry-runs, replay exports, and launcher baseline are usable with manual setup and limited runtime validation.

**Public release readiness: approximately 50%.** The remaining gap is primarily clean-room runtime validation, dependency/prerequisite bootstrap, signing, and loader/edition coverage rather than missing UI screens.

| Area | Status | Notes |
|---|---:|---|
| Native protocol, HUD, telemetry, replay, local SQLite | 90% | V1/V2 tests, TLM1, markers, summaries, session stats, replay reader/writer, opt-in outbox |
| Fabric bridges | 80% | 9 projects build; runtime game validation remains |
| NeoForge bridges | 78% | 6 projects build; V2 parser coverage added; runtime game validation remains |
| Launcher pipeline and UI | 94% | Reference shell, adaptive Home/Library/Screenshot/World galleries, player-facing status, live player-readiness checks, category Settings, Java runtime selection, Servers, downloads, staged creator-pack updates, recovery-first profile/content actions, reversible content restore, hardening, prerequisites, device login, and account UI; clean install remains |
| Quilt/Forge/Bedrock | 82% | Forge bridges now cover 1.12.2 through 1.20.1; cached dry-runs pass, while clean Bedrock coverage remains |
| Mods and AI | 88% | Hash-aware mod/.mrpack install and export, ownership metadata, staged published-pack updates, recovery-first local changes, provider/loader-correct AI pack installs, update path, endpoint/mod regression tests, and live Modrinth/CurseForge installs |
| Packaging/release | 73% | Fresh validated clean archive, SHA-256 component manifest and release manifest, SBOM, prerequisites, and CI workflow; signing/bootstrap remain |
| Friend beta | 40% | Local opt-in outbox and HTTPS contract exist; remote service and delivery acknowledgements do not |

## Verified Now

- Native C++ build succeeds.
- 36/36 CTest suites pass, including native module, camera projection, telemetry replay, profile archive export, archive update-diff classification (Modrinth added/removed, CurseForge manifest flagging, replaced/unchanged cases), performance profiles/options, local content import, Bedrock addon validation, player-readiness modeling, encrypted-config migration, Admin password/config persistence, mods, AI policy, prerequisite, UI model/state contracts, server providers, Essentials, version-catalog selection, server transport, and Forge/NeoForge bridge coverage.
- The latest candidate passes the clean extracted-package runtime smoke test
  (`tools/validate-package-runtime.ps1`): fresh extraction, Java detection,
  prerequisite checks, and a secret scan.
- The current launcher executable embeds a Per-Monitor-V2 DPI manifest. Direct
  renderer captures confirm it uses the physical render surface rather than a
  Windows bitmap-scaled capture; the final visual matrix covers Home,
  minimum-size Home, Discover, Library, profile detail, Downloads, and Settings.
- Live provider verification completed: Modrinth and CurseForge searches both
  succeeded with the configured credentials, and each provider installed a
  compatible Fabric 1.21.1 mod into separate isolated folders with its source,
  project, and version saved in `amalgam-dependencies.json`.
- A local Fabric 1.21.1 dry-run completed successfully with the launcher-owned
  Java 21 runtime, verified client/assets, extracted natives, injected bridge,
  and verified Fabric API; it deliberately did not spawn the game.
- This workstation has no registered `Microsoft.MinecraftUWP` Bedrock package,
  so the Bedrock adapter has automated coverage but cannot receive a live
  launch/import verification here. No Microsoft Java account is currently
  stored; live account validation must be completed by that account owner.
- A live `--doctor --online` check on this workstation verified the native
  bridge, all 19 packaged Java bridges, installed/cached Java runtimes, and
  both provider catalogs. It correctly reports Microsoft account connection as
  the Java launch action and Bedrock installation as an optional edition setup.
- All 9 Fabric projects build from current source.
- All 6 NeoForge projects, Forge 1.20.1, and Forge 1.18.2/1.19.2/1.12.2 projects build from current source.
- Forge 1.18.2 and 1.19.2 now include the maintained Freecam camera action path and Forge mixin configuration, matching the maintained 1.20.1 bridge behavior.
- A controlled Forge 1.20.1 real launch reaches the Minecraft singleplayer world; this machine then exits on an invalid OpenAL audio device. The native DLL owns the Insert control panel across supported loaders; Forge no longer registers a duplicate Java-only panel.
- Sequential cached dry-runs pass for Fabric 1.21.8, Quilt 1.21.8 (pinned to Quilt Loader 0.20.0-beta.9), NeoForge 1.21.8, Forge 1.12.2, Forge 1.18.2, Forge 1.19.2, and Forge 1.20.1.
- `tools/package-release.ps1` creates a clean archive with 19 runtime bridge jars, a credential-free config template, a valid SHA-256 manifest, and accurate native prerequisites. The Modrinth export/import round trip is covered by CTest.
- The latest feature-code pass archive in `dist/` was freshly built after the
  staged-update and recovery-first feature pass and passed
  `tools/validate-package.ps1`; it includes all 19 expected bridge jars,
  branding, an SBOM, component manifest, and no launcher credential file.
- `dist/amalgam-aaaa-release-candidate-20260815-r2.zip` was rebuilt after the
  final Per-Monitor-V2 DPI, renderer-capture, Minecraft-art, and layout pass.
  Its isolated validation passed with all 19 bridge jars, supplied/generated
  branding, hashes/SBOM, and no secret-bearing `launcher.json`.
- The current `dist/amalgam-final-launcher-candidate.zip` carries the supplied neon-violet Amalgam PNG/SVG branding, unified Home/Library and Discover/Browse workflows, creator-manifest modpack installation, the completed profile wizard, and the published-surface credential cleanup; the generated Inno installer installs/uninstalls 19 bridges plus all four branding assets in an isolated smoke test.
- Launcher background state now uses atomic activity flags and synchronized snapshots for authentication, Java discovery, mod jobs, project errors, pack planning, and upstream update status.

## Main Blockers

1. Microsoft/Xbox/Minecraft Services login is implemented through CLI device code, Settings UI, DPAPI storage, and launch integration; live account validation still requires the user’s Microsoft account.
2. Cached sequential dry-runs pass for Fabric, Quilt, NeoForge, and Forge; clean-room runtime launches and Bedrock runtime validation remain. This machine does not have the Bedrock UWP package installed.
3. Native JNI/module behavior and Freecam behavior lack live game integration tests; multiplayer module policy still needs explicit validation.
4. Quilt is pinned but not proven in a clean environment, and Bedrock is a basic UWP/addon adapter.
5. A Windows CI workflow now stages the exact 19 bridges, packages the archive, compiles the installer, uploads artifacts, and checks signing status; it has not run in this environment. The freshly validated launcher and DLL are currently unsigned, so code signing/bootstrap remain public-release blockers.
6. Remote diagnostics ingestion is only a documented HTTPS contract; the local outbox intentionally does not upload.
7. The implemented replay is a redacted telemetry replay, not a full Minecraft world replay. Full world replay playback remains out of scope.
8. Archive preview reports the full added/replaced/removed/unchanged managed-content diff before commit, with regression coverage for Modrinth, CurseForge, and classification edge cases. CurseForge manifests identify remote files by project/file id rather than final provider filename, so those names are resolved during staging; ownership-tracked local profile updates are implemented.
