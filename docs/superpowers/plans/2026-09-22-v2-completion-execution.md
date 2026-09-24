# Amalgam V2 Completion Execution Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use subagent-driven development or inline execution with review checkpoints. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the existing Amalgam V2 repository as far as code and the available environment permit toward a secure, stable, fully verified private-beta release candidate.

**Architecture:** Preserve the established C++ launcher, Java/Bedrock bridge, Supabase, and installer boundaries. Prioritize typed safety boundaries and feature-local fixture facades over broad rewrites; every substantive change receives targeted tests, a fresh native build, and relevant visual/runtime evidence before the next release gate.

**Tech Stack:** C++17/MSVC/Ninja/CTest, ImGui/OpenGL/Win32, Gradle Java bridges, Bedrock add-on validation, Node tooling, Supabase SQL/Edge Functions, PowerShell release automation.

**Spec:** User-provided full V2 completion brief (2026-09-22) and [risk-first stabilization plan](2026-09-22-risk-first-stabilization.md).

## Global Constraints

- Preserve the user’s dirty working tree and never reset, delete, or overwrite unknown artifacts.
- Do not fabricate production account, Cloud, server, provider, friend, session, billing, or performance data.
- Keep fixture visual QA hermetic: no real configuration, account, provider, filesystem, game, Bedrock, or server access.
- Treat live service checks as optional only when existing credentials/authorization permit them; report unavailable external prerequisites precisely.
- Do not weaken checksum, update, RLS, auth, or secret-handling protections to make a test pass.
- Keep the dark near-black/purple Amalgam design system and one-primary-scroll desktop layout.

---

### Task 1: Close high-impact action and fixture isolation gaps

**Files:**
- Modify: `cpp/launcher/src/admin_ui.cpp`, `cpp/launcher/src/cloud_ui.cpp`, `cpp/launcher/src/mod_manager_ui.cpp`, `cpp/launcher/src/ui.cpp`, relevant feature headers/state files
- Test: existing/new CTest model or unit tests in `cpp/tests/`

**Interfaces:**
- Produces a typed, copied-data confirmation boundary with final expiry/auth/target validation.
- Produces fixture-first feature facades that return before live managers/providers/filesystem access.

- [ ] Replace one-click high-impact actions with typed confirmation request/dispatch paths.
- [ ] Enforce administrative session expiry before page render and final dispatch; label account-scoped operations accurately.
- [ ] Disable unsupported control-plane actions honestly rather than exposing a false destructive affordance.
- [ ] Add local fixture states for each changed confirmation/menu and targeted regression tests.
- [ ] Build, execute focused tests, and capture exact fixture smoke evidence.

### Task 2: Complete the core runtime, updater, installation, and recovery audit

**Files:**
- Inspect/modify as audit evidence requires: `cpp/launcher/src/{launch,java,services,updater,extract,instances,config,readiness}.*`, loader/provider helpers, CMake tests
- Test: affected CTest targets and documented Gradle build paths

**Interfaces:**
- Produces a verified runtime artifact/download contract, safe launch argument construction, bounded recovery/error paths, and consistent install/launch state.

- [ ] Trace every supported Vanilla/Fabric/Quilt/Forge/NeoForge path from selection through Java/natives/args/logs.
- [ ] Fix all proven repository-local P0/P1 defects and add regression tests at pure boundaries where possible.
- [ ] Exercise safe dry-run/preflight/probe paths and distinguish them from a real Minecraft launch.
- [ ] Verify updater integrity, rollback/recovery handling, and configuration persistence without weakening validation.

### Task 3: Complete backend, auth, Essentials, and data-security review

**Files:**
- Inspect/modify as evidence requires: `supabase/migrations/`, `supabase/functions/`, `cpp/launcher/src/{supabase,account_manager,essentials_*,social_ui}.*`, verification tools
- Test: Node/Supabase-source verification and any safe configured live checks

**Interfaces:**
- Produces a source-verifiable RLS/function-privilege contract and secure, bounded account/session/Essentials behavior.

- [ ] Inventory final effective table grants/RLS/policies and `SECURITY DEFINER` privileges/search paths across migrations.
- [ ] Fix proven authorization, secret, session, rate-limit, privacy, or cleanup defects using narrowly scoped migrations/code.
- [ ] Verify source contracts and, only when already authorized, live endpoints without printing credentials.
- [ ] Reconcile UI claims with actual backend availability; unsupported Cloud/TURN/control-plane work stays honestly gated.

### Task 4: Finish meaningful launcher UI surfaces and AAA-quality evidence

**Files:**
- Modify: feature-local UI/fixture files and `tools/launcher-visual-qa-cases.full.json`
- Test: `tools/capture-launcher-visual-qa.ps1`, `tools/review-launcher-visual-qa.ps1`, deterministic snapshot captures

**Interfaces:**
- Produces an explicit registry/manifest for every active page/tab/wizard/dialog/menu/state, each with local deterministic presentation data and a reviewable ledger.

- [ ] Expand the existing 165 route matrix with every meaningful confirmation, overflow/menu, auth wizard, Java, Bedrock, server, profile/library, mod-manager, and state-variant surface.
- [ ] Remove demonstrated blank/oversized/incoherent UI and accessibility defects; preserve normal live behavior.
- [ ] Run clean host-valid full matrices at the largest actual client size and compact size; inspect top/middle/bottom and responsive/modal/menu states.
- [ ] Generate immutable review galleries and fix/rebuild/recapture every demonstrated defect.

### Task 5: Release artifacts, performance, and final certification

**Files:**
- Inspect/modify only where evidence requires: `installer/`, `tools/`, `website/`, `README.md`, packaging scripts, planning/release decision docs
- Test: fresh CMake/CTest, Gradle/Bedrock/Node checks, secret scans, package/installer checks, safe runtime probes

**Interfaces:**
- Produces a conservative private-beta GO/NO-GO decision grounded in fresh build, test, visual, security, and packaging evidence.

- [ ] Audit and fix release-tooling, documentation/product-truth, performance, concurrency, and package integrity defects.
- [ ] Run every locally available test/build/check from fresh artifacts and investigate failures.
- [ ] Separate source/build/automated/live/visual verification levels and list only genuine external blockers.
- [ ] Produce the final release decision after the matrix and security/release gates are complete.

## Execution order

1. Task 1 (in progress) blocks every later claim.
2. Tasks 2 and 3 run in parallel source-audit lanes; fix findings in priority order.
3. Task 4 begins only after fixture safety is proven and invalidates/replaces old captures after any shared visual change.
4. Task 5 runs only from fresh final binaries and does not call a release ready while a repository-local P0/P1 remains.
