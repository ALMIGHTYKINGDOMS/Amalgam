# Amalgam Release Completion and Controlled Certification Implementation Plan

> **For agentic workers:** Execute this plan inline with review checkpoints. Steps use checkbox syntax for tracking.

**Goal:** Convert the current locally polished Amalgam launcher into a traceable, self-contained private-beta candidate and close every repository-resolvable release blocker without touching the official website or live Minecraft network.

**Architecture:** Treat the current Release executable, immutable visual ledger, package scripts, and test suites as one provenance chain. Use disposable state for every installed-copy or recovery check. Each system receives an explicit status: locally verified, externally blocked, intentionally gated, or defect found and repaired.

**Tech Stack:** C++17/MSVC/Ninja/CTest, ImGui/Win32, PowerShell packaging and visual-QA tools, Node.js tooling, Gradle Java bridge projects, Bedrock add-on validators, Supabase source verifier, Inno Setup.

**Spec:** [2026-09-23-release-completion-and-controlled-certification-design.md](../specs/2026-09-23-release-completion-and-controlled-certification-design.md)

## Global Constraints

- Preserve the dirty working tree and never reset, delete, or mass-format unknown changes.
- Before every CMake build, inspect active ninja, link, cl, and cmake processes.
- Build only with the repository's documented JDK 17 and Visual Studio developer environment.
- Never access the official Amalgam website or the live Amalgam Minecraft network.
- Never print, write, infer, or package private credentials, account tokens, signing keys, or personal configuration.
- Use a new disposable directory for package/install/first-run checks; do not mutate normal launcher data.
- Any source/binary change invalidates prior package evidence. Rebuild, repackage, rehash, and repeat the affected certification checks.

---

### Task 1: Reconcile current provenance and create the final defect ledger

**Files:**
- Read: final visual evidence directories, Release launcher binary, visual fixture manifest
- Modify: task_plan.md, findings.md, progress.md, this plan's checklist as work completes

**Interfaces:**
- Consumes: current launcher binary, visual ledger JSON/Markdown, fixture manifest, existing release plans.
- Produces: one current status table that distinguishes final local evidence from historical/stale artifacts.

- [x] **Step 1: Inspect the final visual ledger and review summary.**

  Read the local JSON/ledger summaries against the final evidence directory. Record capture totals, failure totals, provenance hashes, and actual client dimensions.

- [x] **Step 2: Fingerprint the binary and manifest used by that evidence.**

  Calculate SHA-256 for the Release launcher and compare it with the ledger's recorded launcher fingerprint. Compare the manifest copy/hash and accepted fixture inventory/count.

- [x] **Step 3: Consolidate only remaining defects.**

  Create entries only for a currently reproducible local defect or a named external dependency. Mark prior fixture/modal/visual items as FIXED + RETESTED only if their final evidence is current.

- [x] **Step 4: Update persistent planning state.**

  Set the current phase and one next action in task_plan.md; write factual evidence to findings.md; append actions/results to progress.md.

### Task 2: Audit the release candidate inputs and operational dependency map

**Files:**
- Read: installer/, tools/, cpp/CMakeLists.txt, cpp/launcher/, cpp/build-release/bridges/, bedrock/, java/, java-neoforge/, java-forge/, supabase/
- Modify if a current repository defect is found: the smallest owning source/test/tool file plus its regression test

**Interfaces:**
- Consumes: current build outputs and package/release tooling.
- Produces: a requirement matrix for launcher core, Java/bridges, Bedrock, servers, providers, Supabase/Essentials, Cloud, updater, and installer.

- [ ] **Step 1: Inventory shipped and runtime-required files.**

  Use file inventory and package-script inspection to map the executable, DLLs, fonts/assets, bridge JARs, Bedrock package, runtime agent, configuration template, updater material, and installer inputs. Mark each required, optional, generated, or externally configured.

- [ ] **Step 2: Trace developer-machine assumptions.**

  Search current production package/input paths for source-tree paths, build-tree paths, developer Java PATH assumptions, manual JAR instructions, localhost, test account markers, and unbounded relative paths. Classify every hit as intentional test-only, package-only, fixed, or actionable defect.

- [ ] **Step 3: Verify honest feature gating.**

  Confirm that Cloud, unconfigured provider/backend, absent Bedrock, missing Java, and unavailable real-session paths render explicit setup/unavailable states rather than Ready or fabricated data.

- [ ] **Step 4: Repair only proven defects.**

  For a repository-resolvable defect, first add or run the narrowest reproducible test, then patch the owning component, rerun that test, and record the exact retest in the defect ledger.

### Task 3: Run current local source, security, and integrity gates

**Files:**
- Read/test: cpp/build-release/, cpp/tests/, tools/update-manifest.test.mjs, tools/verify-supabase.mjs, tools/runtime-agent/, bedrock/AmalgamBedrockClient/
- Modify only when a fresh test identifies a repository-resolvable defect

**Interfaces:**
- Consumes: current source and test suites.
- Produces: a fresh result set that does not rely on historical test counts.

- [x] **Step 1: Run the exact native CTest batches.**

  In the Visual Studio developer environment run the project-prescribed 1–22 and 23–44 CTest batches. Expected current result: 22/22 plus 22/22 passed.

- [x] **Step 2: Run Node and source-contract checks.**

  Run the update-manifest suite, the runtime-agent test suite, and the Supabase source verifier. Do not configure or contact a production Supabase project.

- [x] **Step 3: Run Bedrock package validation.**

  Run the repository's Bedrock validate/package checks. Verify the version, manifests, dependencies, scripts, and assets are internally consistent without importing to a real world.

- [x] **Step 4: Run package-scoped secret/debug scans.**

  Use the repository scanner against source/package staging paths. Record counts and classes only; never print a secret match value.

### Task 4: Produce a fresh, traceable local candidate

**Files:**
- Build: cpp/build-release/
- Read/execute: release and packaging scripts under tools/ and installer/
- Output: a newly named local staging directory under dist/ or artifacts/

**Interfaces:**
- Consumes: green source gates and current package scripts.
- Produces: a candidate package/installer staged from the exact current binary and a SHA-256 inventory.

- [ ] **Step 1: Inspect active build processes before building.**

  Run the documented process check. Do not begin a competing build if a relevant process is already running.

- [ ] **Step 2: Build the documented Release target.**

  Set AMALGAM_JAVA_HOME to the documented JDK 17 installation and run the documented Visual Studio/Ninja command. Record the output binary timestamp and SHA-256.

- [ ] **Step 3: Stage package inputs from that binary only.**

  Use the existing release tooling rather than manually copying files. Verify staging includes the matching launcher, DLLs, bridge JARs, required assets, and intended configuration templates, but excludes source/build/test debris and private data.

- [ ] **Step 4: Generate candidate evidence.**

  Calculate the candidate SHA-256, list its files with relative paths/sizes/hashes, and validate that the staged launcher fingerprint matches the final build fingerprint.

### Task 5: Perform disposable installed-copy certification

**Files:**
- Execute: newly staged candidate/installer only
- Output: a unique local certification artifact directory
- Preserve: normal launcher data, existing installation, profiles, worlds, servers, downloads, and config

**Interfaces:**
- Consumes: current packaged candidate.
- Produces: a CLEAN MACHINE SIMULATED result, not a claim of cross-machine testing.

- [ ] **Step 1: Create and validate a disposable test root.**

  Resolve one unique absolute staging/install/data path. Confirm it is outside the normal launcher data path and record it before any installer action.

- [ ] **Step 2: Run installer/package preflight and inventory checks.**

  Verify correct branding/version/input paths, mandatory runtime files, no unexpected DLL gaps, and no accidental dependency on a source working directory.

- [ ] **Step 3: Launch the installed copy from multiple entry points.**

  Test direct executable, shortcut/start-menu route if generated, and a neutral working directory. Confirm startup, assets, fonts, diagnostics, and safe first-run/empty states without a terminal or missing DLL dialog.

- [ ] **Step 4: Restart and recovery proof.**

  Close normally, relaunch the installed copy, and confirm its disposable configuration remains valid. If a safe repair path exists, damage only a disposable non-user asset and confirm the repair behavior preserves test state.

- [ ] **Step 5: Uninstall/reinstall lifecycle.**

  In the disposable scope only, verify program files/shortcuts are removed as intended while the documented user-data policy is preserved. Reinstall and verify first-run behavior again.

### Task 6: Exercise safe feature journeys from the fresh candidate

**Files:**
- Execute: installed candidate and existing local test fixtures
- Read: launcher logs generated inside the disposable certification root
- Modify only after a reproducible repository defect is found

**Interfaces:**
- Consumes: installed copy and isolated data.
- Produces: truthful local live/fixture verification without a production account or game session.

- [ ] **Step 1: Run launcher diagnostics and prerequisite checks from the installed copy.**

  Verify the doctor/preflight reports core files, Java state, Bedrock absence/detection, backend configuration status, and Cloud gating without revealing secrets or developer paths.

- [ ] **Step 2: Run safe Java/loader checks, then a consented Java profile handoff.**

  Exercise check-java, supported launch preflight/dry-run paths, and bridge/package inventory from the installed copy. The user has authorized a Java Edition test through their already signed-in official-launcher profile, but the currently active Java Minecraft session must never be disrupted. First prove handoff and preflight without touching that session; a real Java launch is allowed only after the active game is closed or the user explicitly asks to replace it. Record the result as a Java-only test, never as Bedrock evidence.

- [ ] **Step 3: Bedrock runtime is explicitly excluded from this certification pass.**

  The user reports Bedrock can crash this PC. Do not detect, launch, import, activate, or otherwise interact with Minecraft for Windows, Bedrock worlds, or add-ons. Retain only the already completed static package validation and metadata integrity checks; do not treat that as a runtime pass.

- [ ] **Step 4: Run controlled local-server and recovery checks.**

  Use only the project’s disposable transport/runtime fixture or a new disposable test server directory. Confirm traversal rejection, console behavior, graceful stop, error presentation, and no access to real server directories.

- [ ] **Step 5: Recheck final visual evidence after any UI-affecting fix.**

  Any visual/shared-layout source change requires a rebuild and replacement capture ledger for affected routes. No old screenshot may certify a changed binary.

### Task 7: Close discovered local defects and recertify

**Files:**
- Modify: the smallest owning production file and exact related test/tool file identified by the failed task
- Update: package evidence and the defect ledger

**Interfaces:**
- Consumes: a reproducible certification failure.
- Produces: FIXED + RETESTED evidence for the original failure plus regression coverage.

- [ ] **Step 1: Reproduce the failure with disposable state.**

  Record the exact command/UI sequence, expected behavior, actual behavior, affected file boundary, and severity before changing code.

- [ ] **Step 2: Add the narrowest regression guard.**

  Prefer a unit, CTest, Node, package verification, or deterministic fixture check that fails before the repair and proves the exact issue afterward.

- [ ] **Step 3: Implement the smallest root-cause repair.**

  Keep private configuration out of source and preserve user data. Avoid unrelated refactors and duplicate UI workarounds.

- [ ] **Step 4: Rebuild, repackage, and repeat the same test.**

  Treat every binary/package change as invalidating the affected candidate evidence. Refresh its hash, inventory, and installed-copy test result.

### Task 8: Assemble the controlled external certification packet

**Files:**
- Modify: findings.md, progress.md, final release report artifact

**Interfaces:**
- Consumes: all completed local evidence.
- Produces: exact asks for only resources that cannot be safely created in code.

- [ ] **Step 1: List genuine external prerequisites.**

  Separate disposable Amalgam/Supabase test account and deployed project; entitled Java Minecraft test account; Minecraft for Windows plus disposable world; configured CurseForge/backend; second Essentials participant/device; signing identity/timestamping; controlled updater endpoint/upload authority; clean Windows VM or second machine.

- [ ] **Step 2: Write the test recipe for each prerequisite.**

  State the minimum action, expected observable outcome, rollback/disposable-state plan, and what result would count as pass/fail. Do not request production website/network access.

- [ ] **Step 3: Issue the final local readiness decision.**

  Report local defects fixed, fresh build/test/package/install results, final candidate fingerprint, externally blocked tests, and either local private-beta readiness or a precise NO-GO reason.

## 2026-09-23 integrated repair gate

Before Task 4 may begin, the current source batch must pass one coherent build and regression run after all of the following have either been fixed and tested or explicitly rejected as an external-only prerequisite:

- full-component, path-safe updater apply/rollback rather than an executable-only swap;
- local-server Ready-state proof, including explicit EULA consent, a regular runnable target, and preservation of imported owner-managed configuration;
- diagnostics that report only local state unless the user deliberately requests an external refresh;
- desktop-client rejection of privileged backend/service keys, with only a public client key eligible for runtime use;
- Bedrock archive digest agreement with its inventory and SHA record in the packager;
- rebuilt/re-staged bridge and Bedrock inputs whose timestamp/hash/version provenance matches the exact candidate.

The audit found a complete 19-jar bridge name set but four stale staged Forge jars, plus an internally self-consistent Bedrock package that predates the current source. These facts are a local candidate-input NO-GO, not permission to alter a semantic release identity. A new isolated output root may use the existing `1.0.0` identity for non-public certification only if the regenerated Bedrock metadata uses that exact value; any public version change remains an intentional release decision.

## Execution order

1. Tasks 1–3 establish current truth and can begin immediately.
2. Task 4 starts only after source/security gates are green.
3. Task 5 uses only the newly staged candidate and disposable state.
4. Task 6 runs after installed startup succeeds.
5. Task 7 loops only if a concrete local defect is discovered.
6. Task 8 is the final handoff and never substitutes an external blocker for a software fix.
