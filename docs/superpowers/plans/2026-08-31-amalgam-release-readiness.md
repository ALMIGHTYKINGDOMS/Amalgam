# Amalgam Release Readiness Completion Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close every repository-resolvable finding from the 2026-08-31 audit and produce evidence for a real beta/release decision without claiming external integrations or gameplay paths that were not exercised.

**Architecture:** Work in six independently testable gates. First make provider/account configuration explicit and safe, then harden version resolution and content installation, correct the Java route, exercise real runtime journeys, fix only demonstrated layout problems, and finally certify the packaged release on a clean environment. Each gate ends with targeted tests plus the complete regression suite.

**Tech Stack:** C++17/MSVC, CMake/Ninja, Dear ImGui, Modrinth API, CurseForge API or approved proxy, Supabase Auth/configuration, Java Gradle bridge projects, Bedrock `.mcaddon` packaging, PowerShell QA scripts, Inno Setup.

**Spec:** `findings.md` and the project requirements in `AGENTS.md`.

## Global Constraints

- Provider credentials and service keys live in runtime configuration or deployment secrets, never in source code, generated screenshots, test fixtures, or git.
- Modrinth remains usable without a token; CurseForge must show a clear unavailable state when no key/proxy is configured.
- Microsoft/Xbox login remains separate from the Amalgam account and must not be represented as connected until the approved client ID and live callback flow succeed.
- Supabase launcher and website must use the same project and compatible Auth configuration before account sync is marked passed.
- Preserve the 19 supported Java bridge artifacts and existing version-specific API handling.
- No custom Minecraft packets; all in-game actions continue through the existing vanilla packet whitelist.
- A release feature is complete only after its success path, failure path, cancellation path, and clean shutdown path are tested.

---

### Task 1: Make release configuration and provider health explicit

**Files:**
- Modify: `cpp/launcher/src/config.cpp`, `cpp/launcher/src/config.h`
- Modify: `cpp/launcher/src/online_config.cpp`, `cpp/launcher/src/online_config.h`
- Modify: `cpp/launcher/src/provider_config.h`
- Modify: `cpp/launcher/src/readiness.cpp`, `cpp/launcher/src/readiness.h`
- Modify: `cpp/launcher/src/settings` surfaces in `cpp/launcher/src/ui.cpp`
- Test: `cpp/tests/config_test.cpp`, `cpp/tests/readiness_test.cpp`
- Validate: `tools/verify-supabase-live.ps1`, `tools/verify-production-schema.mjs`, `tools/secret-scan.mjs`

**Interfaces:**
- Configuration loading must return a typed provider status for Microsoft, Supabase, Modrinth, and CurseForge rather than allowing UI code to infer readiness from empty strings.
- Readiness must expose a user-safe reason and a remediation action without returning or logging credential values.

- [x] **Step 1: Add failing configuration/readiness tests** for missing provider values, valid public values, and secret redaction in diagnostics output.
- [x] **Step 2: Run the focused tests** with `ctest --test-dir cpp/build-vs -R "config|readiness" --output-on-failure` and confirm the new cases fail before implementation.
- [x] **Step 3: Implement typed provider readiness** in the existing configuration/readiness path. Keep keys in runtime configuration only, redact values in logs, and distinguish “not configured,” “network unavailable,” “provider rejected request,” and “ready.”
- [x] **Step 4: Update Settings and Discover messaging** so an unavailable provider has a direct setup action and never looks like an empty catalog or a broken search.
- [x] **Step 5: Run focused tests, the secret scan, and the Supabase schema checks**; record the provider statuses without recording credentials. Live Supabase health passed with the supplied publishable key.

**Exit gate:** Provider status is deterministic, secrets are not printed, and the UI clearly distinguishes configuration gaps from zero search results.

### Task 2: Restore full provider-backed discovery

**Files:**
- Modify: `cpp/launcher/src/mods.cpp`, `cpp/launcher/src/mods.h`
- Modify: `cpp/launcher/src/net.cpp`, `cpp/launcher/src/net.h`
- Modify: `cpp/launcher/src/ui.cpp`, `cpp/launcher/src/ui_model.h`
- Test: `cpp/tests/mods_test.cpp`, `cpp/tests/net_test.cpp`
- Validate: the live Modrinth and CurseForge provider endpoints through the configured runtime path

**Interfaces:**
- Discovery results must carry provider, project type, supported game versions, supported loaders, download count, icon, and provider URL.
- Search responses must preserve partial-provider success: Modrinth results remain visible while CurseForge reports its unavailable reason separately.

- [x] **Step 1: Add tests** for provider merge, duplicate project identity, partial failure, pagination, and provider-specific project URLs.
- [x] **Step 2: Run `ctest --test-dir cpp/build-vs -R mods --output-on-failure`** and verify the new tests fail where the current behavior is incomplete.
- [x] **Step 3: Implement provider result normalization and partial-failure reporting** without hardcoding project data or credentials.
- [x] **Step 4: Exercise Discover with empty search, a known mod, a known modpack, each content-type filter, each provider filter, and a no-result query. Modrinth was live; CurseForge remains unconfigured.
- [ ] **Step 5: Confirm real CurseForge results in a separately configured runtime** and capture only counts/status, not the key.

**Exit gate:** Discover loads useful content before a query, searches both configured providers, keeps one provider usable when the other is unavailable, and opens a full detail page for every result type.

### Task 3: Make version and loader resolution install-safe

**Files:**
- Modify: `cpp/launcher/src/version_catalog.h` and its implementation
- Modify: `cpp/launcher/src/import_pack.cpp`, `cpp/launcher/src/import_pack.h`
- Modify: `cpp/launcher/src/launch.cpp`, `cpp/launcher/src/launch.h`
- Modify: `cpp/launcher/src/java.cpp`, `cpp/launcher/src/java.h`
- Modify: `cpp/launcher/src/mods.cpp`
- Modify: `cpp/launcher/src/server_manager.cpp`, `cpp/launcher/src/server_manager.h`
- Test: `cpp/tests/version_catalog_test.cpp`, `cpp/tests/import_pack_test.cpp`, `cpp/tests/java_test.cpp`, `cpp/tests/mods_test.cpp`
- Test: `tools/validate-cached-launches.ps1`

**Interfaces:**
- Version selection must be driven by canonical catalog entries with Minecraft version, loader, loader version, Java requirement, server artifact strategy, and bridge availability.
- Install planning must return a resolved plan or a structured incompatibility report before downloading files.

- [x] **Step 1: Add failing tests** for exact version matching, loader aliases, unsupported bridge fallback, Forge/NeoForge/Fabric/Quilt selection, Java requirement selection, and server-jar availability.
- [x] **Step 2: Run the focused version/import/java tests** and confirm the missing cases fail.
- [x] **Step 3: Implement a single resolver** used by profile creation, modpack installation, server creation, and launch. Reject random text versions at the UI boundary and populate selectable options from the resolver.
- [x] **Step 4: Add preflight output** listing the selected Minecraft version, loader, loader version, Java runtime, bridge artifact, server artifact, and any missing dependency before installation begins.
- [x] **Step 5: Run cached-launch validation** across the catalog and verify that every supported entry has a deterministic success or an honest unsupported explanation. The seven-case matrix passed for Fabric, Quilt, NeoForge, and Forge across 1.12.2, 1.18.2, 1.19.2, 1.20.1, and 1.21.8.

**Exit gate:** Modpack/profile/server/version controls cannot request an unresolvable combination, and a failed plan explains exactly which artifact or compatibility rule is missing.

### Task 4: Complete real content installation and launch journeys

**Files:**
- Modify: `cpp/launcher/src/mods.cpp`, `cpp/launcher/src/import_pack.cpp`, `cpp/launcher/src/launch.cpp`
- Modify: `cpp/launcher/src/downloads` behavior in `cpp/launcher/src/ui.cpp` and related download state code
- Modify: `cpp/launcher/src/diagnostics.cpp`, `cpp/launcher/src/crash_report.cpp`
- Create or modify: `tools/qa-content-install.ps1`
- Test: `cpp/tests/import_pack_test.cpp`, `cpp/tests/updater_test.cpp`, `cpp/tests/diagnostics_outbox_test.cpp`

**Interfaces:**
- An install job must expose queued, active, completed, failed, cancelled, and retryable states with a stable job id.
- Retry must resume or safely restart a failed artifact without duplicating files or leaving a half-installed profile marked playable.

- [x] **Step 1: Add a scripted fixture test** for a pack with dependencies, a missing optional dependency, a checksum mismatch, cancellation, retry, and successful completion.
- [x] **Step 2: Run the fixture tests** and confirm failure before implementation.
- [x] **Step 3: Implement transactional profile staging**: download to temporary files, verify size/hash where available, move into the profile only after all required artifacts pass, then write the playable marker.
- [x] **Step 4: Update Downloads** so failed rows have Retry and Remove actions, completed rows can be cleared, and a failed install never disappears without an explanation.
- [ ] **Step 5: Run one real Modrinth modpack install** in a disposable profile, launch it, exit cleanly, and verify the profile/library/download state afterward.
- [ ] **Step 6: Run one real CurseForge modpack install** in a separately configured runtime and record whether the provider path, dependencies, and launch succeed.

**Exit gate:** At least one real pack from each configured provider installs and launches, while failure/retry/cancel behavior is safe and visible.

### Task 5: Give Java Edition its correct route and polish account flows

**Files:**
- Modify: `cpp/launcher/src/ui.cpp` around `kPlayNav`, `draw_shell`, `draw_java_tab`, and page dispatch
- Modify: `cpp/launcher/src/ui_state.h`, `cpp/launcher/src/ui_model.h` only if a distinct Java route needs state
- Modify: `cpp/launcher/src/auth_wizard.cpp`, `cpp/launcher/src/auth_wizard.h`, `cpp/launcher/src/account_ui.cpp`
- Test: `cpp/tests/ui_model_test.cpp`, `cpp/tests/auth_test.cpp`
- Validate visually: `tools/qa-native-run.ps1` plus the live desktop route

**Interfaces:**
- Clicking Java Edition must render a Java Edition overview/management surface, not the generic Library surface.
- Every account/profile wizard must close through its top-right close control, Escape, Cancel, and its final action without leaving a stale modal state.

- [x] **Step 1: Add a UI model test** asserting distinct navigation ids for Java Edition, Library, and Bedrock Edition.
- [x] **Step 2: Run the focused UI/auth tests** and confirm the Java-route assertion fails on the current mapping.
- [x] **Step 3: Correct the navigation mapping and page dispatch** while preserving the existing Library surface and profile selection behavior.
- [x] **Step 4: Add modal lifecycle tests** for open, close, cancel, validation error, success, and reopen-after-close.
- [x] **Step 5: Visually test Home → Java Edition → Library → Bedrock Edition and account sign-in/register flows** at the supported window sizes. Java Manager and the route matrix render correctly; authentication remains externally gated.

**Exit gate:** Java Edition has the correct destination and all tested wizards close reliably without restarting the launcher.

### Task 6: Certify Servers, Essentials, Downloads, and Bedrock runtime paths

**Files:**
- Modify: `cpp/launcher/src/server_manager.cpp`, `cpp/launcher/src/server_manager.h`, `cpp/launcher/src/server_ui.cpp`
- Modify: `cpp/launcher/src/essentials_manager.cpp`, `cpp/launcher/src/essentials_session.cpp`, `cpp/launcher/src/essentials_ui.cpp`
- Modify: `cpp/launcher/src/bedrock.cpp`, `cpp/launcher/src/bedrock.h`, `cpp/launcher/src/bedrock_ui.cpp`
- Modify: `cpp/launcher/src/ui.cpp` for tab state and empty/error states
- Test: `cpp/tests/cloud_hosting_test.cpp`, `cpp/tests/essentials_test.cpp`, `cpp/tests/bedrock_test.cpp`, `cpp/tests/server_providers_test.cpp`
- Validate: `bedrock/AmalgamBedrockClient/tools/validate.js`

**Interfaces:**
- Local server state must distinguish detected, installing, starting, online, stopped, failed, and externally managed.
- Bedrock state must distinguish package ready, install required, repair required, detected game, launch unavailable, and launched.

- [x] **Step 1: Add tests** for local server import/create/start/stop/restart state transitions, Essentials host/join/stop flows, and Bedrock install/repair/detect error states.
- [x] **Step 2: Run the focused server/Essentials/Bedrock tests** and confirm the missing transitions fail.
- [x] **Step 3: Implement the minimum state transitions and actionable error surfaces** using existing providers and package artifacts.
- [x] **Step 4: Run the Bedrock validator and install the generated `.mcaddon` into a disposable Bedrock test environment.
- [ ] **Step 5: Test Bedrock offline launch, online account-gated launch, addon import, repair, and clean exit where the installed game permits it.
- [ ] **Step 6: Create a disposable local server, acquire the correct server jar, start it, inspect console/logs, stop it, restart it, and verify the library state.

**Exit gate:** The launcher can explain and complete the available server/Essentials/Bedrock paths instead of presenting controls that only look functional.

### Task 7: Remove unnecessary scrolling and finish visual regression

**Files:**
- Modify: `cpp/launcher/src/ui.cpp`
- Modify: `cpp/launcher/src/ui_shell.cpp`
- Modify: `cpp/launcher/src/ui_components.cpp`
- Modify: `cpp/launcher/src/ui_helpers.cpp`
- Modify: `cpp/launcher/src/ui_motion.cpp`, `cpp/launcher/src/ui_motion.h` only where animation/layout timing causes clipping
- Test: `cpp/tests/ui_model_test.cpp`
- Validate: live screenshots at minimum, default, and wide launcher window sizes

**Interfaces:**
- Outer shell scrolling is reserved for content longer than the viewport; child panels own scrolling only when their content is independently long.
- Modal footers remain visible while modal content scrolls, and close controls remain reachable at every supported size.

- [x] **Step 1: Capture a route matrix** for Home, Discover, project details, Library, Downloads, Essentials, Servers, Settings, Bedrock, every wizard, and every tested secondary tab at the minimum supported size. The 22-route 960x600 matrix rendered and exited successfully; wizard lifecycle coverage is recorded under Task 5.
- [x] **Step 2: Add a layout regression checklist** to the UI test notes covering clipped controls, empty-state centering, horizontal overflow, child scroll ownership, and modal footer visibility. See `docs/ui-layout-regression-checklist.md`.
- [x] **Step 3: Fix only the demonstrated scroll ownership/clipping defects** and preserve legitimate scrolling on long detail, settings, cloud, and log content. Added `NoScrollWithMouse` to the shared sidebar child after reproducing compact-window wheel capture; focused 960x600 interaction and post-fix route captures passed.
- [x] **Step 4: Re-run the route matrix with screenshots** and compare against the supplied launcher/client reference artwork for hierarchy, spacing, branding placement, and density. No new accidental clipping or unnecessary scroll defect was demonstrated.

**Exit gate:** No page scrolls when its content fits, no control is clipped or hidden behind a child scroll region, and long content remains reachable.

### Task 8: Build and certify the public package

**Files:**
- Modify only as needed: `tools/build-release.cmd`, `tools/build-vs-release.cmd`, `tools/package-release.ps1`, `tools/build-installer.ps1`, `installer/AmalgamLauncher.iss`
- Validate: `tools/validate-package.ps1`, `tools/validate-package-runtime.ps1`, `tools/qa-installer-inputs.ps1`, `tools/qa-launcher-smoke.ps1`, `tools/verify-signing.ps1`
- Document: `docs/release-checklist.md`

**Interfaces:**
- The release package must contain `amalgam_launcher.exe`, `amalgam.dll`, the validated Bedrock package, exactly the validated bridge set, required runtime artwork, and a configuration template with no embedded secrets.
- The installer must support install, repair/upgrade, uninstall, shortcuts, and clean first launch without developer-machine paths.

- [x] **Step 1: Run the full release build** with `tools/build-release.cmd` and confirm the staged release contains the native binaries, Bedrock package, bridge jars, runtime art, and package metadata.
- [x] **Step 2: Run `tools/validate-package.ps1 -Package dist/plan-2026-08-31-fixed-sidebar/amalgam-1.0.0.zip`** and fix every missing or mismatched artifact.
- [x] **Step 3: Run `tools/validate-package-runtime.ps1 -Package dist/plan-2026-08-31-fixed-sidebar/amalgam-1.0.0.zip`** from a clean temporary extraction directory and fix every runtime-only dependency issue.
- [x] **Step 4: Build the Inno Setup installer** with `tools/build-installer.ps1 -SourceDir dist/plan-2026-08-31-fixed-sidebar/amalgam-1.0.0 -Version 1.0.0 -OutputDir dist/plan-2026-08-31-fixed-sidebar/installer`.
- [ ] **Step 5: Install, upgrade, launch, close, and uninstall on a clean Windows test account**; verify shortcuts, writable data directories, config migration, and rollback behavior. Disposable install/launch/repair-upgrade/silent-uninstall lifecycles passed with config preservation; separate-account rollback remains open.
- [x] **Step 6: Run installer-input, launcher-smoke, signing, secret-scan, CTest, and Bedrock validation again after packaging. Signing remains advisory because no certificate is installed.
- [x] **Step 7: Write the release decision** with explicit PASS, BLOCKED, or NOT TESTED status for Microsoft, Supabase, CurseForge, Modrinth, Java, Bedrock, servers, and the installer. See `docs/release-decision-2026-08-31.md`.

**Exit gate:** The package passes structural and runtime validation on a clean environment, and any external approval/configuration dependency is clearly documented rather than hidden.

## Self-review checklist

- Provider configuration and account connectivity are covered by Tasks 1–2.
- Versions, loaders, server jars, dependency installation, retries, and playable markers are covered by Tasks 3–4.
- The Java Edition route and all wizard close paths are covered by Task 5.
- Servers, Essentials, Downloads, and Bedrock runtime journeys are covered by Task 6.
- Scroll ownership and reference-image visual QA are covered by Task 7.
- Release packaging, Inno Setup, clean-machine checks, and the final decision are covered by Task 8.
- No task requires embedding or printing a secret.

Plan complete and saved to `docs/superpowers/plans/2026-08-31-amalgam-release-readiness.md`. Two execution options:

1. Subagent-driven execution — dispatch a fresh worker per task with review checkpoints.
2. Inline execution — execute tasks in this session with checkpoints.
