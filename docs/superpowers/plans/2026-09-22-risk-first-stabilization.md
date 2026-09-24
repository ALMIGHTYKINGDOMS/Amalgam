# Amalgam Risk-First Stabilization and Polish Plan

> **For agentic workers:** execute each task in order, preserve unrelated working-tree changes, run the listed checkpoint before advancing, and treat unavailable credentials/deployment source as explicit release blockers rather than a reason to weaken a check.

**Goal:** Eliminate the audited repository-resolvable P1 reliability and release-gate defects first, then harden security/product truth, refine visible UX, and collect fresh evidence for a fact-based release decision.

**Architecture:** The work is delivered as four gates. Gate A makes filesystem, name, JVM-command, and artifact-integrity behavior deterministic. Gate B makes release/security verification accurately measure the full source and effective Supabase policy state. Gate C separates empty, filtered, and non-cancellable states in the product and aligns checked-in claims with evidence. Gate D rebuilds and tests the actual artifacts, then distinguishes passed paths from external blocks.

**Tech stack:** C++17/MSVC/CMake/Ninja, Dear ImGui, Node built-in test runner, PowerShell release scripts, Supabase SQL migrations and source verification, Gradle Java bridges, Bedrock validator, Inno Setup.

**Approved design:** `docs/superpowers/specs/2026-09-22-risk-first-stabilization-design.md`.

## Guardrails

- Do not reset, clean, discard, broadly format, or accidentally stage the existing September working tree.
- Keep provider keys, OAuth client secrets, Supabase keys, CurseForge keys, signing material, and machine-specific paths out of source, logs, screenshots, and reports.
- Preserve server serialized enum values, packet whitelist behavior, bridge protocol layouts, and public install formats.
- New tests must be deterministic and offline unless a task explicitly identifies a disposable live verification path.
- A change that affects a user-visible claim must use a current source of truth or downgrade/remove the claim.
- Build/test commands run under the configured Visual Studio environment and reference `cpp/build-release`; never infer success from an old binary or a no-op incremental graph alone.

---

## Gate A — P1 local-server stabilization

### Task 1: Establish regression coverage before behavior changes

**Files:**
- Modify: `cpp/tests/server_run_state_test.cpp`
- Modify: `cpp/tests/server_provision_test.cpp`
- Modify: `cpp/tests/server_providers_test.cpp`
- Modify/create only if a pure launch-command helper needs its own existing test target: `cpp/tests/services_test.cpp` or the narrowly related CMake test registration

**Interfaces under test:**
- `server::validate_server_name(const std::string&, std::string*)` (new pure helper).
- `server_provision::prepare_runtime_directories(...)` or an equivalent explicitly testable non-throwing internal boundary.
- A pure command/argument construction helper for loader starts if extracting it is required to prove Windows quoting.
- A complete `RuntimeArtifact` passed into the runtime download wrapper.

- [ ] Add name-validation assertions before implementation: a normal spaced name succeeds; empty/whitespace-only names, dot aliases, path punctuation, controls, trailing space/dot, `CON`, `PRN`, `AUX`, `NUL`, `COM1`–`COM9`, `LPT1`–`LPT9`, and device names with extensions fail with a useful reason.
- [ ] Add provisioning assertions before implementation: a server directory nested below an existing ordinary file returns false and a nonempty error without throwing; a normal directory yields the runtime and scratch directories.
- [ ] Add loader command assertions before implementation: Forge/NeoForge user JVM and win-args paths beneath a directory such as `Forsaken World SMP` each appear as one Windows command-line argument beginning with `@`; a path without whitespace retains normal readable output.
- [ ] Add artifact-contract assertions before implementation: a synthetic artifact with SHA-1, SHA-256, and byte count maps all three fields into the request passed to the network wrapper; an artifact with none maps no invented verification data.
- [ ] Run the focused existing tests and record the expected gaps/failures before implementation. If tests cannot compile until their target helper exists, add the helper as the smallest first implementation step and use the first focused run as the red/green boundary.

**Checkpoint:** `ctest --test-dir cpp/build-release -R "server_(run_state|provision|providers)|services" --output-on-failure` under the configured MSVC environment.

### Task 2: Make server names and creation failures safe on Windows

**Files:**
- Modify: `cpp/launcher/src/server_types.h`
- Modify: `cpp/launcher/src/server_ui.cpp`
- Modify: `cpp/tests/server_run_state_test.cpp`

**Implementation details:**
- Add an inline/pure `validate_server_name` beside the server data contract, returning a boolean and optional user-safe explanation. Use ASCII case normalization only for Windows-reserved-name matching; do not rewrite a user’s display name.
- Reject names that are empty after trim, include a control or Windows path-invalid character, equal `.`/`..`, end in a dot or space, or whose case-insensitive base before the first dot is a DOS device name. Preserve names with ordinary embedded spaces.
- Replace the create-dialog ad hoc boolean with this helper; show its returned explanation and retain the existing case-insensitive duplicate-name guard.
- Use `std::filesystem::create_directories(path, error_code)` in the create action. On failure do not push a broken server record or close the dialog; keep the form open and display the filesystem reason in the existing error surface.
- Let `write_server_config_files` remain the only follow-on writer after directory creation succeeds, preserving its error behavior.

- [ ] Implement the pure validation helper and replace the create-dialog predicate.
- [ ] Replace throwing UI directory creation with the error-code form and user-visible recovery text.
- [ ] Add/adjust tests for every invalid family and a valid spaced display name.
- [ ] Run the focused server-state test and visually inspect the validation/error state at the launcher’s supported minimum size.

**Checkpoint:** invalid inputs cannot enable Create; an induced filesystem failure leaves the launcher usable and explains why it could not create the directory.

### Task 3: Contain provisioning filesystem failures

**Files:**
- Modify: `cpp/launcher/src/server_provision.h`
- Modify: `cpp/launcher/src/server_provision.cpp`
- Modify: `cpp/tests/server_provision_test.cpp`

**Implementation details:**
- Add a narrow `prepare_runtime_directories` boundary, or a functionally equivalent testable routine, that creates the server directory then `.amalgam\\runtime` with error-code overloads.
- On either error, return false and provide a concise message that identifies the failed runtime directory and the operating-system error. Do not catch a broad exception around the worker; prevent one from being thrown.
- Call this boundary after successful runtime resolution and manual-runtime rejection, so manual providers do not create stray directories merely to report their manual instruction.
- Continue using error-code removal where a cleanup failure is nonfatal; do not turn cleanup-only misses into install success/failure changes without a test.

- [ ] Add the directory preparation contract with its two explicit error paths.
- [ ] Route `provision_runtime` through it in every runtime kind.
- [ ] Add a file-as-parent test and a success-layout test; verify no exception escapes.
- [ ] Run the provisioning target before running the combined server suite.

**Checkpoint:** a path conflict returns an actionable error from `provision_runtime`/its helper and leaves no launcher crash path.

### Task 4: Preserve all published artifact integrity metadata

**Files:**
- Modify: `cpp/launcher/src/server_providers.h`
- Modify: `cpp/launcher/src/server_providers.cpp`
- Modify: `cpp/launcher/src/server_provision.cpp`
- Modify: `cpp/tests/server_providers_test.cpp`
- Modify: `cpp/tests/server_provision_test.cpp` only if the request helper is located there

**Implementation details:**
- Change `download_runtime` to take a `const RuntimeArtifact&` rather than a URL plus SHA-256. It must pass `artifact.url`, `artifact.sha1`, `artifact.size`, and `artifact.sha256` to `net::download` in its existing parameter order.
- Add `companion_size` to `RuntimeArtifact` and retain the published vanilla size when Fabric resolves its companion server jar. Build a local companion artifact before passing it through the same download contract.
- Update Jar, LoaderJar, Archive, and Installer call sites so no path bypasses the artifact wrapper accidentally. A provider that publishes no checksum/size continues with empty/-1 verification inputs and must not be called verified in new UI text.
- Prefer a pure `runtime_download_request`/equivalent structure if the current network layer is not mockable; test values at that boundary instead of adding test-only network hooks.

- [ ] Extend the artifact data contract and resolver assignment for Vanilla and Fabric companion metadata.
- [ ] Refactor download calls to use the complete artifact and remove the lossy signature.
- [ ] Add parser/resolver/request tests covering SHA-1 plus size, SHA-256, both values, and absent values.
- [ ] Run provider/provisioning tests, then run the full native suite.

**Checkpoint:** no provider-published hash/size gets dropped before `net::download`; all existing `.part`/checksum behavior remains green.

### Task 5: Quote loader argument files and make cancellation truthful

**Files:**
- Modify: `cpp/launcher/src/services.cpp`
- Modify: `cpp/launcher/src/services.h` only if extracting a testable command builder requires a declaration
- Modify: `cpp/launcher/src/server_ui.cpp`
- Modify: `cpp/launcher/src/ui_state.h` or the existing runtime-download state owner only if a cancellability flag does not already exist
- Modify: relevant native test file(s), minimally `cpp/tests/server_run_state_test.cpp` or `cpp/tests/services_test.cpp`

**Implementation details:**
- Build loader argfile tokens as an at-prefix plus a Windows-quoted path (`@` + `extract::quote(path)`), so process parsing receives one argument whose first character is `@`. Do not add duplicate memory flags outside `user_jvm_args.txt`; retain exactly one source of the requested allocation.
- Verify writing `user_jvm_args.txt` fails cleanly before attempting process launch, and report that exact error to the caller.
- Introduce a pure, testable loader command/arguments helper if direct process construction cannot be asserted without creating a child process.
- Add `cancellable` to the runtime job state. Set it true for resolving/downloading/unpacking steps; set it false before invoking a loader installer. The UI replaces Cancel download with a disabled, explanatory “Installing — cannot cancel safely” state during that phase. Completion is success if installation succeeds even if an earlier cancel flag was set after it became non-cancellable.
- Preserve cooperative cancellation for transfer callbacks and the existing cancelled outcome for a transfer actually stopped before completion.

- [ ] Implement and test at-file quoting with a server directory containing spaces.
- [ ] Remove duplicated/ambiguous memory flags and add write-failure handling for the JVM args file.
- [ ] Add phase-driven cancellability; update both list and detail action surfaces and the progress copy.
- [ ] Run focused server/service tests and a dry-run or controlled fixture with a spaced loader path.

**Checkpoint:** Forge/NeoForge start arguments are parse-safe on ordinary user paths, and no UI state promises installer cancellation it cannot deliver.

### Gate A regression gate

- [ ] Rebuild native Release from the edited sources in the configured MSVC environment.
- [ ] Run all CTest targets with output on failure.
- [ ] Run `git diff --check` and inspect only files touched by this gate for accidental churn.
- [ ] Capture a minimum-size Servers create/error state plus a ready/detail state, using absolute artifact paths.

---

## Gate B — release tooling and Supabase security evidence

### Task 6: Correct multi-root secret scanning

**Files:**
- Modify: `tools/secret-scan.mjs`
- Create: `tools/secret-scan.test.mjs`
- Inspect/modify only if needed: `tools/release-gate.ps1`

**Implementation details:**
- Parse `process.argv.slice(2)` as all requested roots; default to the current working directory only when no root is supplied.
- Resolve roots, ignore missing paths only when the existing scanner’s documented behavior explicitly permits it, walk each requested root, and aggregate findings into one nonzero result. Avoid scanning the same resolved root twice.
- Include a root-relative location that unambiguously identifies a later root in diagnostic output, but never print a full real secret value.
- The Node test creates two temporary fixture roots. It passes a clean first root and a second root containing only a harmless marker matching the scanner category, asserts failure/output for the combined invocation, then asserts the clean root alone passes. Always clean temporary files in a finally block.

- [ ] Add the failing two-root test.
- [ ] Implement all-root aggregation and preserve the existing no-argument behavior.
- [ ] Run `node --test tools/secret-scan.test.mjs`, then `node --test tools/*.test.mjs`.
- [ ] Run the actual three-root release scan (`tools`, `installer`, `cpp/launcher`) and record only counts/outcome.

**Checkpoint:** a match in any argument root fails the gate; a pass means every requested root was examined.

### Task 7: Audit effective Supabase privileges and function search paths

**Files:**
- Inspect: `supabase/migrations/*.sql`, `supabase/functions/**`, `tools/verify-supabase.mjs`, `tools/verify-supabase-live.ps1`
- Create/modify: a deterministic source verifier under `tools/` only after its contract is specified by the migration inventory
- Create: a new sequential Supabase migration only when the audit identifies an effective privilege/search-path defect and the desired roles are known
- Create when tooling is available: `supabase/tests/` policy/function verification assets

**Implementation details:**
- Inventory the final effective DDL across migrations, not isolated earlier statements. For each exposed table record grants, RLS enabled state, policies, intended authenticated/anonymous role, and route that accesses it.
- For each `SECURITY DEFINER` function record schema, owner intent, final `search_path`, `PUBLIC` execute status, explicitly granted roles, and caller input validation. Require a pinned safe path that excludes caller-controlled schemas; use fully qualified object references where appropriate.
- Do not introduce blanket `USING (true)` policies or broad `PUBLIC` function execution as a shortcut. Make any new migration idempotent only where the project convention supports it, and retain an explicit audit trail.
- Extend source verification to fail for known insecure final states. If Supabase CLI/local DB/link credentials become available, add allow/deny tests for anon/authenticated/service behavior and execute advisor checks. Without that route, mark runtime policy testing blocked with exact prerequisites.

- [ ] Produce the effective privilege/function inventory and decide whether a migration is necessary.
- [ ] Add source-verifier coverage for declared security invariants.
- [ ] If and only if the inventory proves a defect with a known intended role, add the narrowly scoped migration plus policy test.
- [ ] Run source verification, then any available local/linked DB test path; do not claim live policy verification from source parsing.

**Checkpoint:** the repository can deterministically demonstrate intended final grants/RLS/function execution/search paths, or it has a precise external DB verification block.

### Gate B regression gate

- [ ] Run the full Node tooling suite, explicit all-root secret scan, and `node tools/verify-supabase.mjs`.
- [ ] Re-run the secret scan after any migration/tooling edit.
- [ ] Record the exact distinction between source verification and live project verification in `findings.md`/release decision material.

---

## Gate C — product truth and UI polish

### Task 8: Give profile Content a purposeful empty and filtered state

**Files:**
- Modify: `cpp/launcher/src/ui.cpp` and any existing content-model/helper file actually used by that route
- Modify: `cpp/tests/ui_model_test.cpp` if the state split is model-testable
- Create/update: deterministic snapshot fixture or UI QA note only if one already exists in the project conventions

**Implementation details:**
- Determine whether the selected profile has zero installed content before filtering. With zero inventory, show “No content installed yet” plus concise context and only available actions (for example Discover/import where those routes exist).
- With inventory but an active search/filter yielding zero, retain the selected filters and show “No installed content matches these filters” plus a clear-filter action.
- Make the search affordance self-explanatory through an in-field hint or close linked label; do not leave a visually blank input with an ambiguous detached label.
- Preserve keyboard navigation, minimum-size layout, loading/error state distinctions, and existing asynchronous cache behavior.

- [ ] Add/adjust model-level tests or deterministic fixtures for no inventory versus filtered empty state.
- [ ] Implement the visual/copy split and action behavior.
- [ ] Rebuild and capture 960x600, default desktop, and wide snapshots where available; inspect for action clipping and misleading text.

**Checkpoint:** a brand-new profile and a filtered nonempty profile communicate different, actionable situations.

### Task 9: Align checked-in claims and documentation with current evidence

**Files:**
- Modify: `README.md`
- Modify: `website/index.html`
- Modify/create: a release/capability evidence document if no canonical source exists

**Implementation details:**
- Correct current test-count statements from fresh suite discovery rather than hand-maintained historic numbers.
- Replace or remove static-site claims that cannot be tied to current catalog/configuration/package evidence, including stale server counts, AI-provider/privacy wording, installer size, and local-AI storage estimates.
- Do not imply no cloud service or a downloadable public installer while the product configuration/download route is intentionally gated. Keep static-site copy honest but do not attempt to patch the separate deployed React site without its source.
- Add a short source-location/blocker note for live-site improvements so follow-up work can resume when the actual deployment source is supplied.

- [ ] Build an evidence table from current catalog, config contract, package manifest, and verification outputs.
- [ ] Update README/static page copy to only state supported, current facts.
- [ ] Re-run source scans and inspect page rendering locally if a local static preview exists.

**Checkpoint:** repository documentation and static marketing copy do not make a claim that this audit cannot trace to current evidence.

### Task 10: Classify release inputs and preserve evidence

**Files:**
- Inspect: root untracked files, `artifacts/audit-2026-09-22/`, `tools/`, `cpp/tests/`, migration directory, ignore rules
- Modify only when classification is clear: `.gitignore`, `tools/`, `cpp/tests/`, `docs/`, or `artifacts/`

**Implementation details:**
- Classify each untracked item as product source, durable test/QA asset, evidence, generated/disposable artifact, or unknown user work.
- Promote a useful source/test only to its appropriate tracked directory after confirming it is deterministic and documented. Never commit raw credentials, private logs, binaries, PID files, or backup data.
- Leave unknown items untouched and report them as release hygiene blockers. Use recoverable cleanup only after a user decision if an item is material and untracked provenance is unclear.

- [ ] Produce a classification table with paths/counts and no sensitive contents.
- [ ] Promote only clearly reusable QA/test code if needed by this plan; leave all other user-owned artifacts in place.
- [ ] Run `git status --short` and `git diff --check` after every promotion.

**Checkpoint:** the final release decision can identify which files are intentionally part of the deliverable and which must be resolved before a clean commit/package.

---

## Gate D — fresh verification and release decision

### Task 11: Rebuild all repository-resolvable artifacts

**Files:** no production edit expected; record evidence in planning/release documentation.

- [ ] Perform a fresh native Release configure/build or a verified clean rebuild path, then run all CTest targets.
- [ ] Build the relevant Java bridge trees with the documented JDK/Gradle versions; copy artifacts only into a disposable/staged location until package validation.
- [ ] Run Bedrock add-on validation and all Node tool tests.
- [ ] Run Supabase source verification and policy/runtime tests available without adding secrets.
- [ ] Run explicit secret scans for every shipped source/config/package root.

**Checkpoint:** every locally executable build/test source is green or has a named, reproducible blocker.

### Task 12: Live, visual, package, and external-gate certification

**Files:**
- Modify/create: `docs/release-decision-2026-09-22.md` after verification
- Update: `findings.md`, `progress.md`, and `task_plan.md`

- [ ] Execute a disposable local-server create/prepare/start/stop/restart path where provider access and Java permit; include a loader path with spaces and test the failure path without risking a user server.
- [ ] Capture launcher route snapshots at 960x600, normal desktop, and wide dimensions permitted by the display, including profile Content and server installer/cancel state.
- [ ] Run package structure/runtime and installer input checks from a newly staged artifact; perform install/upgrade/uninstall only in a disposable target. Do not overwrite a public/release artifact.
- [ ] Check live Supabase/Microsoft/CurseForge/authenticated gameplay/signing/clean-account conditions only when configuration and external approval are supplied. Otherwise list them as BLOCKED with the missing prerequisite.
- [ ] Write PASS / BLOCKED / NOT TESTED status for native, Java, Bedrock, providers, Supabase, authentication, server lifecycle, package, installer, signing, public website, and working-tree hygiene.

**Checkpoint:** final report makes no unsupported “everything is good” claim; it identifies what is demonstrably good and what still requires an external owner or clean environment.

## Plan self-review

- P1 filesystem/name/path/integrity and release-gate defects are first, each with a targeted regression test.
- Security changes are intentionally evidence-led and do not attempt to guess database role intent.
- UI polish follows reliability rather than masking it.
- External credentials, deployment source, signing, and clean-machine conditions remain distinct from repository code quality.
- The existing dirty worktree is protected throughout.
