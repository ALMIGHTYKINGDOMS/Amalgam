# Amalgam Progress Log

## 2026-09-22 — Exhaustive launcher visual-evidence pass

- Reclassified the current scope as a full launcher visual-evidence and polish pass under the already approved risk-first approach; retained the existing release-readiness plan and added Phase 12 for this work.
- Added deterministic named-fixture plumbing to the launcher, including a per-capture artifact root, explicit page-level top/middle/bottom positions, and a strict registry rather than arbitrary route flags.
- Added a comprehensive fixture manifest/matrix. It now matches all 165 registered routes exactly, contains 239 primary 1600×900 capture records (165 top, 27 middle, 47 bottom), and identifies 159 compact and 183 small-desktop minimum review records.
- Added local-only account surfaces and Bedrock review data. Those facades explicitly avoid account/session hydration, Bedrock discovery, provider calls, file dialogs, launches, imports, and mutations; they are evidence for rendering/state composition only.
- Completed a fresh unified MSVC launcher build at `cpp/build-release/amalgam_launcher.exe` (timestamp 2026-09-22 19:05 local) after integrating the fixture work.
- Completed the full fresh native CTest suite: the test log records 39/39 passing tests, including UI model, server transport, and client-bridge checks. Compiler warnings were non-fatal existing-style unused-parameter/internal-helper warnings; no build error occurred.
- Fresh smoke recaptures from the current binary visibly verify the global local-fixture boundary, neutral account navigation state, Data Packs content filter, semantically green healthy TPS progress, and singular Bedrock `world` / `add-on` labels.
- The evidence workstation is physically limited to a 1280×720 desktop. Requested 1600×900 and 1366×768 screenshots clamp below their requested sizes, so those earlier mislabeled artifacts are historical only. The full pass now uses a ledger-validated 1256×624 host-max request (currently 1242×610 client) and a 960×600 stress request (currently 946×586 client), without substituting them for genuine large-host certification.
- Next: execute the full host-valid deterministic matrix with its hash-verified capture ledger, inspect all surface/state/scroll evidence, fix and recapture demonstrated UI defects, then complete the small-screen matrix.

## 2026-09-22 — Full-project audit kickoff

- Restored the existing release-readiness planning context and confirmed there was no unsynced session-catchup output.
- Inspected the branch, recent commits, repository surface, and working-tree state.
- Preserved the existing September server/Supabase/UI change set and marked the older August evidence as historical rather than current.
- Began an architectural audit and refinement design; no product code has been changed in this pass.
- Quantified the September diff and inventoried the current test surface, build directories, placeholder markers, and new provisioning test target.
- Reviewed the provisioning architecture, runtime catalog, async UI job, start-target resolution, and test changes; identified a concrete digest/size verification gap in the Vanilla runtime path.
- The first fresh-build invocation failed before compilation because its nested Windows command quoting was malformed; the toolchain path itself exists, so the next attempt uses corrected shell quoting.
- Corrected the Windows wrapper; the current Release tree is build-current (`ninja: no work to do`). A direct CTest call then exposed a PATH-only environment issue and is being rerun under the developer environment.
- Ran the full current native suite under the verified developer environment: 39/39 CTest targets passed in 42.85 seconds.
- Reviewed the current Supabase changelog and official RLS, function-privilege, API-security, and advisor guidance before continuing the database-security audit.
- Ran the repository Supabase verifier successfully (22 migrations, 24 Edge Functions), confirmed the CLI is unavailable, and identified exposed-schema/search-path hardening as an explicit security workstream.
- Ran the Bedrock validator successfully and audited the README, static website, tools, and ignore rules; recorded stale public claims and documentation counts as release-truth debt.
- Selected the browser QA workflow for public-site validation; its global command is absent, so the next attempt uses the supported on-demand `npx` entry point.
- Loaded the version-matched browser core and dogfood QA instructions, including the issue taxonomy. The QA workflow requires an isolated named session, annotated evidence, console/error checks, and accessibility/responsive coverage.
- Opened the live Amalgam site in an isolated session and captured the first annotated desktop frame plus snapshot/console evidence; a largely blank main viewport is being reproduced before it is recorded as a confirmed issue.
- Reproduced the live-site splash on reload and confirmed the full page appears after waiting; reclassified the symptom as a several-second first-impression delay, not missing content.
- Measured the live page (TTFB 117.6 ms, FCP 316 ms, LCP 1.784 s, CLS 0.40), ran a valid axe audit (three violations), and captured the 390x844 mobile state.
- Visually reviewed the mobile hero and the gated live download page. A menu click probe failed only because PowerShell consumed an unquoted browser ref; the route QA continued successfully.
- The first desktop snapshot batch returned success without producing files at the requested relative paths; no product conclusion was drawn, and the capture path/process behavior is being diagnosed before retrying.
- Launcher logs confirmed all four snapshot routes rendered; the save failure was limited to unresolved relative output directories. Retried the server detail route with an absolute path and obtained the expected PNG.
- Captured and visually reviewed the modified server list/detail and profile-content routes. Server layouts hold at minimum size; the profile Content empty state and search labeling need a focused polish pass.
- Reviewed the release gate and found its multi-path secret-scan invocation silently scans only the first path. Ran the runtime-agent and tooling suites (24/24 and 39/39) and scanned each intended directory explicitly with no secret matches.
- Safely inspected configuration presence without printing credentials: Microsoft client ID is present; Supabase and CurseForge runtime configuration are absent. Direct GUI-subsystem CLI probes did not return exit codes to PowerShell, so no new doctor result was claimed from that invocation.
- Re-ran doctor and prerequisite checks as waited hidden processes; both exited 0. Reviewed the September live-provision evidence and current untracked QA debris to inform cleanup/promotion work.
- Pinned exact source/test evidence for unquoted Forge/NeoForge arg-file paths, Windows-reserved server-name handling, throwing provisioning directory creation, and non-interruptible installer cancellation.
- Completed the September audit and closed the browser QA session. The prioritized risk-first phases are now recorded in `task_plan.md`; product-code implementation remains gated on user approval of the design.
- The user approved Approach A (risk-first stabilization, then polish). Saved the approved design at `docs/superpowers/specs/2026-09-22-risk-first-stabilization-design.md` and the test-first execution plan at `docs/superpowers/plans/2026-09-22-risk-first-stabilization.md`.
- Began Phase 8 in-place because the existing September worktree is intentionally dirty and contains user-owned in-progress changes. A standalone commit/worktree split is deferred until changes can be partitioned safely.

## 2026-08-31 — Fresh audit baseline

- Completed a fresh MSVC Release build in `cpp/build-vs`.
- Completed 36/36 automated tests.
- Completed Bedrock package validator.
- Completed broad live UI smoke coverage across launcher pages, tabs, dialogs, and wizards.
- Fixed and retested the recovery dialog close-state defect.
- Confirmed clean shutdown removes the running marker.
- Remaining work is tracked in `findings.md` and the phased implementation plan under `docs/superpowers/plans/`.

## 2026-08-31 — Phase 1/3 kickoff

- Confirmed the local build configuration has no active provider credentials; the unauthenticated Supabase verifier correctly returned a configuration-dependent HTTP 401.
- Added a testable Java Edition navigation route and corrected the Play sidebar to open the existing Java Manager surface instead of Library.
- The new route assertion was intentionally run before implementation and failed as expected; implementation is ready for the focused rebuild/regression check.

## 2026-08-31 — Release-readiness execution pass

- Supabase live health passed with the supplied publishable key; no credential was written to source or the package.
- Added the typed Java Edition route helper and corrected the Play sidebar to open Java Manager; fresh UI model and full regression tests pass.
- Ran fresh visual snapshots for Java Manager, Servers, Downloads, Essentials, Bedrock, and the installed-package Home route. Long-list scrollbars were confirmed legitimate; no demonstrated clipping fix was required in this pass.
- Installed a real Modrinth mod and a real Fabulously Optimized Modrinth pack into disposable isolated profiles; version, loader, files, and metadata were verified.
- Ran the real server transport fixture against the fresh launcher; files, traversal rejection, console transport, and graceful stop passed.
- Corrected Bedrock UWP detection and current AUMID activation (`!Game`); Bedrock launch returned success and exited cleanly after the test.
- Built strict Release, packaged `dist/plan-2026-08-31/amalgam-1.0.0.zip`, compiled `AmalgamLauncher-1.0.0-Setup.exe`, passed package/runtime/installer-input/secret checks, and passed an isolated installer + native prerequisite + Home smoke check.
- Remaining release gates are explicit: CurseForge configuration, Microsoft approval/authenticated gameplay, configured launcher↔website session persistence, full gameplay launch, clean-account install lifecycle, focused minimum-size scroll matrix, and code signing.

## 2026-08-31 — Plan execution continuation

- Completed the cached-launch matrix: all seven Fabric/Quilt/NeoForge/Forge cases passed across the supported legacy and current versions.
- Captured and visually reviewed 22 routes at the enforced 960x600 minimum window size; all rendered successfully and no new accidental overflow or clipping defect was demonstrated.
- The remaining UI verification is limited to child-scroll interaction at minimum/maximum sizes; external provider, Microsoft approval, account persistence, clean-account lifecycle, gameplay, and signing gates remain explicitly unverified.
- Final regression rerun completed after the plan documentation update: 36/36 CTest passed.
- Clean native real-user QA then completed in one session: Home → Discover → Library → Downloads → Essentials → Servers → Settings, global search, page scrolling, and clean Alt+F4 shutdown all passed; no reproducible clipping or crash was observed.
- Reproduced the remaining compact scroll defect, fixed shared sidebar wheel ownership with `NoScrollWithMouse`, rebuilt, reran the focused 960x600 interaction check, and confirmed the sidebar stays fixed while the selected fitting Settings section does not scroll.
- Rebuilt the updated Inno installer after hardening its silent-uninstall path. A fresh disposable install returned 0, packaged prerequisites were detected, an installed Home snapshot was created, and silent uninstall returned 0 while removing the installed binaries and preserving user-created config/log/snapshot files.
- Removed a stale release-tool assumption: `tools/qa-launcher-smoke.ps1` now accepts `-PackageDir` and was run successfully against the current fixed-sidebar package, confirming prerequisite and Java checks use the intended artifact.
- Completed a second disposable installer lifecycle for repair/upgrade: the first-launch config was preserved byte-for-byte, the upgraded build passed prerequisite checks, and silent uninstall removed installed binaries while retaining the user-created config.
- Final regression snapshot after the continuation pass: all 36 CTest cases passed again in 24.88 seconds.

## 2026-08-31 — Launcher responsiveness pass

- Frame-paced the desktop render loop to approximately 60 FPS; removed the previous effectively uncapped `Sleep(1)` behavior.
- Moved recurring profile content/count, Library-world, and Screenshots-gallery scans off the render thread with cache generations and pending flags to prevent stale worker results from replacing newer selections.
- Rebuilt both `cpp/build-vs` and `cpp/build-release` with Visual Studio MSVC.
- Headless verification passed: 36/36 CTest, extracted-package runtime validation, prerequisite smoke, and Java smoke.
- Packaged the updated artifacts under `dist/plan-2026-08-31-performance/`.
- Deliberately did not open or focus the launcher for live visual QA during this pass because the user was actively playing a game.
# 2026-09-22 — Expanded V2 completion pass

- User expanded the authorized scope from the visual-evidence pass to full Amalgam V2 completion: risk-first stabilization, core runtime/auth/backend/Essentials/Bedrock/release review, implementation, testing, and conservative certification.
- Rejected the first `host-max-full` visual run before the `cloud` route after discovering that route would call live provider APIs. No partial set is being treated as completed evidence.
- Implemented a fixture-first Cloud presentation façade using local representative data only; its verified smoke capture succeeded (`1242x610` actual client area, SHA-256 ledger-style fingerprint) and did not alter the real launcher configuration.
- Began typed Admin destructive-action confirmation work and Quick Search zero-state/privacy polish. Those source changes are not yet claimed as build- or visual-verified because the shared confirmation wiring and targeted tests remain in progress.
- Added `docs/superpowers/plans/2026-09-22-v2-completion-execution.md` and expanded `task_plan.md` with Phases 13–16. Next action: finish all confirmed destructive-action/fixture-safety repairs, build, test, and add their visual fixture routes before restarting a new immutable matrix.

## 2026-09-23 — Stabilization continuation

- Safely contained the Microsoft-auth regression test in a process-owned temporary local-app-data directory, then ran the focused test successfully without recreating a normal user credential file.
- Corrected runtime telemetry semantics and added regressions; the runtime-agent suite is 38/38 green, and local Supabase source verification is green for 25 migrations and 24 Edge Functions.
- Hardened the update-manifest CLI and added payload-integrity tests; its Node suite is 12/12 green.
- Began the shared account-action safety implementation: transactional local session mutations, explicit this-device sign-out scope, target revalidation, identity separation, a confirmed Minecraft disconnect, and truthful local-sign-in copy. These code changes remain pending one coherent native build/test pass.
- Established the current visual-evidence baseline precisely: 53 current broad PNGs exist, but they are stale, top-only, host-clamped, and unledgered; the next evidence run must be a fresh candidate after stabilization.
- Completed the account-action safety pass and wired its deterministic visual fixtures. Amalgam sign-out, removal of remembered local sign-ins, removal of all remembered local accounts, and Minecraft disconnect now have distinct confirmations and truthful effects.
- Completed Java/native launch hardening: exact Java-major validation, explicit process executable selection, deterministic native layout fingerprinting, safe staged extraction/recovery, and traversal-safe exclusions. The focused Java/extraction/native tests passed.
- Completed the release-tooling ordering repair and its isolated fixtures without using a certificate, private key, public update endpoint, or deployment. The release workflow now distinguishes deliberate inert local candidates from a production release candidate.
- Ran the complete current native suite in two bounded batches after the shared source settled: 18/18 and 24/24 passed (42/42 total).
- Verified that the fixture source registry and visual matrix remain synchronized after the new account-dialog routes: 172 source tokens, 246 primary planned captures, 166 compact high-risk captures, and 190 small high-risk captures.

## 2026-09-23 — Risk-first source integration in progress

- Extended fixture composition without touching a real account, credential, provider, local Bedrock installation, or server folder. The source matrix now declares 203 exact launcher fixture routes, 277 canonical visual states, and 695 planned scroll/viewport captures.
- Hardened the capture runner around an executable-exported fixture inventory. It now fails before artifact creation when the binary’s exact token set, manifest token set, declared count, or hashes disagree; this intentionally rejects the pre-expansion executable.
- Added safe representative visual states for the entire account creation/verification/recovery and Microsoft device-code journey, plus local-server file deletion, backup restore, and saved-entry removal confirmations/errors.
- Repaired the Local Servers remove-entry path so the detail screen cannot bypass its confirmation, and made failed local persistence/file deletion explicit and retryable rather than silently closing the user’s decision point.
- Started the final render-thread stabilization batch: authenticated account actions and Essentials/social actions are moving through joined, generation-guarded request lanes. No claim of build/test completion is made until those concurrent source changes are integrated and validated together.

## 2026-09-23 — Visual evidence hardening and exhaustive-surface expansion

- Completed a source-only capture-runner reliability repair. The runner now retries only a clean-exit exact-1x1 artifact or material viewport clamp once by default in a fresh isolated environment, preserves all attempt artifacts, and exposes them in schema-v3 JSON and Markdown evidence. Static PowerShell parse/trailing-whitespace checks passed.
- Executed and verified a focused normal runner proof: Home and Essentials/Friends at requested 960x600 and 1256x624 passed 4/4 with one attempt each. Executed an intentional 1600x900 clamp proof: two separately isolated 1242x610 attempts were retained and rejected, and the capture correctly failed.
- Began an all-state two-tier capture run, then intentionally stopped only its verified QA process tree after 91 artifacts when a fresh audit showed that the otherwise synchronized 221-token/296-case registry omits meaningful active modal/menu and recovery surfaces. The partial run is retained under `artifacts/audit-2026-09-22/launcher/all-surfaces-local-post-polish-retry-hardened-*`; it is diagnostic only, not visual certification.
- Performed a focused active-surface audit and identified a fixture-safety P1: existing Server remove-entry and file-delete snapshot overlays can reach live mutation handlers if clicked; backup restore reports an inert error but remains enabled. Next: fail-close all fixture-mode mutation controls/entry points, extend local-only routes to the uncovered dialogs/menus, then rebuild and re-run the expanded immutable matrix.

## 2026-09-23 — Expanded interaction proof integration

- Completed the fixture-safety repair for Server destructive previews. Fixture controls are visibly disabled and action entry points return before local persistence, file removal, or backup service calls.
- Added local-only presenters and registry coverage for the previously unrepresented interaction states, including profile recovery/version/creator dialogs, library groups, project install conflicts, screenshot/mod menus, friend profile outcomes, cache cleanup, Admin actions, managed Java, and Server failure/file-preview states.
- Expanded `tools/launcher-visual-qa-cases.full.json` from 296 to 337 records and synchronized its declared source inventory from 221 to 261 tokens. JSON parsing, case-ID uniqueness, route parity, and `node tools/update-manifest.test.mjs` (12/12) passed.
- Pending: compile and execute the full native suite, verify the compiled fixture inventory, run two-tier focused visual proof, then start a fresh complete immutable 337-case evidence ledger. No global visual-completion claim has been made.

## 2026-09-23 — Final local visual closure and release-completion planning

- Completed the source integration, fresh Release build, and split native regression suite after the expanded fixture work. CTest passed 22/22 plus 22/22, and the manifest update test passed 12/12.
- Captured the final complete local visual matrix under artifacts/ui-polish-final-sticky-2026-09-23/ui-polish-final-sticky-20260923T170821235Z-f59e4cd6: 792/792 passed, with zero failed/blocked captures and zero transient retry/recovery.
- Generated the final review gallery under artifacts/ui-polish-final-sticky-reviews-2026-09-23/review-ui-polish-final-sticky-20260923T170821235Z-f59e4cd6-20260923T182553996Z-5e9e4f5d: 257 contact sheets; all 792 images valid and provenance-consistent.
- Visually inspected the final compact high-risk recovery/modal states. The last Essentials fixture dialog clipping was repaired with a fixed footer plus scrollable detail region; the real join dialog received the equivalent layout correction. Rebuilt/retested/recaptured evidence is current.
- User asked to plan and begin every remaining item. Added an approved-scope release-completion design and an execution plan that begin with provenance/dependency/package audits and proceed only through disposable local candidate testing. Official website and live Amalgam Network access remain excluded.

## 2026-09-23 — Release-completion execution started

- Restored the planning state, created the controlled-certification design/implementation plan, and began its provenance/dependency audit.
- Read the final capture/review ledgers. Current binary/manifest hashes still match the 792/792 evidence. The previously staged ZIP/installer is demonstrably stale and will not be overwritten or reused.
- Performed the required active-build-process check, then built cpp/build-release; Ninja reported no work needed. Fresh native test batches passed 22/22 and 22/22.
- One initial CTest invocation used a malformed developer-environment path and produced no test output. The exact corrected documented command was used next; LastTest.log and the second batch confirm all 44 tests passed. Do not repeat the malformed path.
- Ran fresh local non-production checks in parallel: update-manifest 12/12, runtime-agent 38/38, Supabase source verification (25 migrations/24 Edge Functions), Bedrock truth-correctness 4/4, and Bedrock validation passed.
- Operational audit identified two P1 repository defects (multi-component updater swap/rollback and local-server Ready truthfulness) plus diagnostic truthfulness/performance P2. Scoped repairs have been dispatched; no external website, live network, account, provider, or user data was accessed.

## 2026-09-23 — Release-blocker repair review started

- Read and revalidated the approved controlled-certification design and execution plan, then began executing it in reviewable batches with isolated local state only.
- The server-provisioning repair is now ready for root review: explicit EULA consent is threaded through provisioning/configuration, installer-style runtimes must resolve to a regular launch target before Ready, and new tests cover the missing-consent and missing-target paths. It has not yet been built or claimed as passing.
- The updater repair remains in active implementation. An independent read-only audit confirmed the full-package/EXE-only swap mismatch and supplied the required manifest-controlled transaction and rollback test cases.
- Dispatched a separate read-only audit of bridge/Bedrock package freshness so a future candidate is assembled only from current components. No release artifact has been overwritten.
- Reviewed the server provisioning patch and tightened the shared launch-target resolver: path names are not sufficient evidence of a runnable runtime; the expected target must be a regular file. Added isolated regression cases for directory impostors across JAR, Forge arguments, and Bedrock executable forms. This source batch is awaiting the required build/test pass.
- Ran the current secret-scanner regression suite (2/2 passed) and a scoped source/package-input scan over 522 text files. It reported zero secret-category matches and no unexpected HIGH findings without printing any credential material. The future staged candidate still requires its own package scan.
- Traced the Prepare call path and repaired a data-loss risk: imported server `server.properties` and existing accepted `eula.txt` are preserved rather than rewritten by runtime repair. Added an isolated byte-for-byte preservation regression; this joins the current server build/test batch.

## 2026-09-23 — Parallel hardening progress

- Reviewed the completed diagnostics repair. It replaces misleading synchronous remote checks with local configuration/session facts and testable policy results; compile and regression verification are queued with the integrated source batch.
- Completed a read-only bridge/Bedrock freshness audit. It found 19/19 bridge names, four stale Forge staged copies, and an internally consistent but source-stale Bedrock package. No existing distribution or generated artifact was changed.
- Started two additional narrow repairs in parallel: desktop client rejection of legacy privileged backend keys, and Bedrock archive/hash metadata verification in the package tool. Both are limited to local source/tooling and inert tests.
- The full-package updater transaction remains the only active P1 implementation not yet ready for root review. Native builds stay paused until that writer finishes so one coherent source tree is tested.
- A first documentation append used stale tail context and was rejected without changing files; the relevant plan files were reread and updated with precise independent patches.
- User added a Java-only live-test authorization through an already signed-in official launcher profile, while explicitly excluding Bedrock runtime activity due to a reported PC crash. A read-only Windows-app inspection found an active Java Minecraft session; no input was sent to it, and later test steps now require a non-disruptive handoff/preflight first.
- Integrated C++ Release build completed, followed by both mandatory CTest halves: 22/22 plus 22/22 passed. The new updater transaction, server provisioning/state, diagnostics, and desktop key-contract checks all compiled and passed.
- Re-ran local source/release gates after the repairs: update manifest 12/12, runtime agent 38/38, Supabase source verifier 25 migrations/24 Edge Functions, scanner tests 2/2 plus 522-file scan clean, and package-script parsing clean. No production endpoint, account, Bedrock runtime, website, or live network was used.
- Next concrete local gate: rebuild/re-stage Java bridge and static Bedrock package inputs, then create an isolated candidate package. Bedrock runtime remains excluded.
- Bridge-refresh environment findings: the documented offline Gradle cache lacked Fabric Loom 1.17.19, so the local-only retry failed before compilation; the old directory labeled JDK 21 actually runs Java 17, so Fabric Loom correctly refused it. The verified local JDK 21.0.12.1 toolchain then completed a 9-target Fabric build successfully. A serialized `--rerun-tasks` pass is now regenerating artifact provenance; no source, user data, Minecraft session, or Bedrock runtime has been touched.

## 2026-09-23 — Official 1.0.0 launch-readiness pass started

- User requested full launch readiness. Applied the launch-gate plan: the goal is a fresh immutable `1.0.0` candidate and an honest final GO/NO-GO, not a premature public promotion.
- Read the official-launch, operational-readiness, final-triage, release-certification, and planning-with-files instructions. The run will preserve the active Java Minecraft session, keep Microsoft approval as an external gate, and perform no Bedrock runtime activity.
- Identified the current visual matrix as structurally synchronized but incomplete for tooltips, open combos, toasts, theme variants, and DPI coverage. These are now tracked as launch-evidence closure items, with Bedrock-labelled routes excluded from the final capture selection.
- Current native build completed successfully and the full CTest suite passed: 45/45 tests, including isolated local Bedrock truth checks only.
- Supporting gates passed: runtime-agent 38/38, update-manifest 12/12, Supabase source verification (25 migrations / 24 Edge Functions), scoped secret scan (no unexpected HIGH), Bedrock static truth 4/4, and Bedrock validator.
- Fresh Java-only visual evidence completed in four isolated partitions: 733/733 captures passed at the current post-polish binary, with 733/733 valid image hashes/dimensions in the reviewed ledgers. Contact-sheet review completed for all partitions with zero images needing integrity attention.
- Immutable candidate staged at `artifacts/release-candidate-2026-09-23/rc-20260923T150234478Z`: package validation, extracted runtime validation, installer-input QA, candidate secret scan, Inno Setup build, Java-only preflight, official handoff probe, and disposable install/check/uninstall all passed.
- Candidate hashes: ZIP `27D6039304FBE7D01CECAACE15A86CC4BA0B4DB72F10D8C411BE4FEE2F9F7757`; installer `0974982121D972AAD9D35112660E311BCA01908EAB14877F0E3FAFA82CD25F0C`; launcher `4B005BD22012CB2E27EC6F4913EE52042D7885EB5C22BE4794F2D941ACDA7779`; DLL `AB7A209992904B13659CAC0502598D52E4705855F1C2BB26735C434C9DEAE5EE`.
- Public launch remains externally gated: Microsoft production approval, trusted Windows code signing/timestamping, authorized update payload hosting/key verification, live Supabase/provider deployment, and authenticated real Java handoff are not claimed. Bedrock runtime remains intentionally untested.
- Earlier focused capture was stopped before certification because it was still using a stale pre-fix binary and had no completed ledger. Its partial files remain diagnostic only.

## 2026-09-23 — Latest post-polish certification checkpoint

- Completed the current final-polish build and full native regression: `cpp/build-release` is current and CTest passed 45/45 after the transient fixture/theme changes.
- Refreshed the full Java-only visual evidence at host-valid requested sizes 1256x624 and 960x600. The four partition ledgers under `artifacts/ui-polish-java-only-full-2026-09-23-refresh` contain 751/751 passed captures (190, 186, 190, 185); every reviewed gallery reports zero integrity-attention images. Manual inspection covered the current light, midnight, and dark shell treatments, open theme picker, error toast, and launcher home composition.
- Added deterministic transient evidence for the open five-theme selector and polished transient states. The dedicated picker review contains 2/2 valid images and zero integrity attention.
- Re-ran support gates: runtime-agent 38/38, update-manifest 12/12, Supabase source verification 25 migrations/24 Edge Functions, Bedrock static truth 4/4 plus validator, and the scoped source secret scan. The candidate package secret scan found no secret-category matches.
- Created a new immutable local candidate at `artifacts/release-candidate-2026-09-23/rc-20260923T231319050Z`. ZIP/package validation, clean-extracted runtime checks, installer-input QA, installer compilation, component-manifest binding, `--check-prereqs`, `--check-java`, `--check-official-launcher`, and the isolated Java official-handoff probe all passed. The long workspace path initially exceeded the handoff probe's Windows path limit; rerunning with a short isolated temp root passed without touching the active Minecraft session.
- Current candidate hashes: launcher `E454EF0A87EFD9C180D40DF81A8E429C176404CDF8AFC277C236F06E8B957DAE`; DLL `AB7A209992904B13659CAC0502598D52E4705855F1C2BB26735C434C9DEAE5EE`; ZIP `341647DDE7A5C330030814C20D57F68BD0952A92167F8074BC3F247D40AD1A0F`; installer `7ACC6B019B0264F06DDEBAC89D8147ACCCB396C842E487C9779DB0596A063E95`.
- The existing local QA installation is still present, so I did not silently replace or uninstall it during this pass. The prior disposable install/uninstall evidence remains valid for the earlier candidate; a new destructive installer simulation should wait until that QA install is explicitly cleared or the user authorizes replacement.
- Public launch remains `TECHNICALLY READY — EXTERNAL OWNER GATE`: native signing/timestamping, Microsoft approval, live hosted update/payload authority, live Supabase/provider configuration, and a real authenticated Java handoff remain outstanding. Bedrock runtime remains intentionally excluded.

## 2026-09-23 — Live backend, release, and repository integration checkpoint

- Applied the live Supabase `rpc_privilege_hardening` migration to project `nnrrmvaxnoknthpwvttt`. The user-bound editor-job RPC now rejects mismatched identities; credit grants and the trigger helper are service-role-only. Post-migration privilege checks confirmed anonymous execution is disabled for all three functions.
- Updated the live verifier for the current Supabase API behavior: it now checks representative authenticated table endpoints instead of the removed anonymous OpenAPI schema probe. Live schema reachability, Auth reachability, and the protected CurseForge route (401 without a session) all pass.
- Built the current Release tree, ran all 45 native tests (45/45), validated the configured `1.0.0` ZIP and clean extraction, and compiled `dist/installer/AmalgamLauncher-1.0.0-Setup.exe` successfully. The candidate embeds the real Microsoft client ID, live Supabase URL/publishable key, and official website URL; no private provider key is shipped.
- Scoped source secret scan reports no unexpected HIGH findings. Repository hygiene now ignores local QA/build debris while preserving source, migrations, tests, visual QA tooling, and release documentation for the project repository.
- Remaining external launch gates are unchanged: Microsoft production approval and entitled Java sign-in, an authenticated CurseForge request, trusted Windows signing/timestamping, publisher-controlled update hosting/signature, clean-machine compatibility/install testing, and a real Java handoff after the active session is closed. Bedrock runtime remains excluded by user instruction.

## 2026-09-23 — Two-pass audit and polish closure

- **Audit pass 1:** rechecked source/runtime/provider wiring, visual-matrix parity, release freshness, PowerShell syntax, production-schema invariants, and the live Supabase project. Found one concrete product mismatch: the branded network default still carried an obsolete `:9564` port.
- Corrected the single online network source of truth to the canonical user-provided address `play.amalgam-network.com` and added a regression assertion so future changes cannot silently reintroduce the stale port.
- **Audit pass 2:** rebuilt the Release target and reran the complete native suite (45/45), runtime-agent suite (38/38), update-manifest suite (12/12), secret-scanner suite (2/2), Bedrock static truth suite (4/4), production-schema verifier (66/66), bridge/package freshness checks, visual-case parity (445 cases / 344 tokens / zero duplicate or missing high-risk references), and launcher CLI preflight checks (all exit 0).
- Revalidated the live Supabase verifier (representative schema tables, Auth, protected CurseForge route), package integrity, clean extracted runtime behavior, and candidate-package secret scan (0 matches). No Minecraft session was disturbed and no Bedrock runtime was launched.

## 2026-09-23 — Approved risk-first UI and local server-management polish

- Repaired the Java Manager layout assertion by removing the nested runtime-card scrollbar and ensuring every missing-runtime row emits a real ImGui item. A fresh host-valid capture shows no red Dear ImGui assertion overlay.
- Tightened the profile-create wizard's hero, preset cards, source controls, and fixed footer so the source step uses its scroll boundary intentionally while target/performance/review remain reachable.
- Refined Bedrock Overview and Profiles empty states with compact, branded copy and a real create-profile entry point. Bedrock runtime was not launched, per the user's PC-crash constraint.
- Made local server Overview telemetry truthful: supervised-process CPU and working-set RAM now come from the Windows process; TPS and player counts remain explicitly unavailable until a server-side health bridge reports them. Fake zero values are no longer rendered as live metrics.
- Confirmed the existing local server controls remain real service-backed operations (start/stop/restart, console/commands, file browser, players, plugins, properties, world/backups) and added the telemetry path without changing fixture safety or authentication/session state.
- Fresh Release build completed; CTest passed 45/45; CLI preflights `--check-prereqs`, `--check-java`, `--check-official-launcher`, and `--bedrock-info` all exited 0.
- Host-valid focused visual proof passed 5/5 at 1242x610 in `artifacts/audit-2026-09-23/ui-polish/final-polish-20260924T022628776Z-dc8c5163`; Java, Bedrock Overview/Profiles, Server Overview, and profile wizard source were inspected. The capture host clamps 1366x768 requests to 1242x610, so the larger tier remains an environment limitation rather than an app failure.
- Remaining external gates are unchanged: user-approved relaunch is required before the already-running launcher window can display the new binary; TPS/player telemetry needs a server-side bridge; Microsoft production approval, trusted signing, update hosting, and live authenticated provider checks remain outside this local pass.

## 2026-09-23 — Final binary/evidence binding

- [x] Changed all `ServerMetrics` signal flags to fail-closed defaults and marked only the visual fixture's representative CPU/RAM/TPS/player values as valid. Rebuilt `cpp/build-release/amalgam_launcher.exe` (SHA-256 `7871F39C267360F2118E2FF71A6569D186A207865AC817FA0BF6737223D39D3E`); CTest remains 45/45.
- [x] Recaptured the affected visual routes against that exact binary: 5/5 passed at host-valid 1242x610, with binary/manifest/inventory/config hashes bound in `artifacts/audit-2026-09-23/ui-polish/final-polish-truthful-metrics-20260924T023338389Z-55865ac3`.
- [x] Started the current build for direct inspection (PID 24232). No Bedrock runtime was started and no official Minecraft session was touched.

## 2026-09-24 — Shared website/launcher account contract

- [x] Confirmed with the owner that the official website and Windows launcher use the same Supabase project (`nnrrmvaxnoknthpwvttt`); no second account system or Microsoft-auth change is required.
- [x] Added canonical account URL helpers for official login, registration, password recovery, and account pages. Browser links are derived from the configured official website origin and use only the fixed `return=launcher` hint; no credentials or tokens are placed in URLs.
- [x] Added launcher actions on the Amalgam welcome, registration, sign-in, and password-reset surfaces so users can use the official website while retaining native Supabase auth in the launcher.
- [x] Added regression coverage for canonical account routes and custom-origin slash normalization; focused config/auth/cloud tests pass and the full native CTest suite remains 45/45.
- [x] Built a separately named verification binary (`cpp/build-release/amalgam_launcher_auth.exe`) because the currently running `amalgam_launcher.exe` (PID 24232) holds the release output open. CLI preflights pass 3/3, and the shared-account visual review ledger contains 13/13 host-valid auth captures at 1242x610.
- [ ] Replace the locked `cpp/build-release/amalgam_launcher.exe` after the owner closes the currently running window, then bind the final release hash and recapture any required auth evidence against that exact filename. This is an environment lock, not a source or test failure.
- [ ] Optional future enhancement: verify and implement the deployed website's one-time `/launcher-auth` exchange contract if automatic browser-cookie-to-native-session handoff is desired. Same Supabase project identity is already complete without that optional bridge.

## 2026-09-24 — Bedrock release gate and Microsoft approval messaging

- [x] Put the Bedrock navigation behind an explicit Coming Soon release gate. The route now renders one compact, branded state that explains Profiles, Worlds, and Add-ons/Backups are in preparation; it does not inspect the local Bedrock installation or expose launch, install, import, profile, world, backup, or add-on actions.
- [x] Kept Bedrock runtime testing excluded as requested. The new state was verified only through deterministic visual fixtures; the existing static Bedrock safety tests remain part of the native suite.
- [x] Updated Microsoft OAuth and wizard copy to accurately identify the remaining Xbox/Minecraft app-approval gate. The existing Amalgam integration is configured; after approval propagates, the next sign-in retry should work without a launcher update or account change. The official Minecraft Launcher fallback remains available meanwhile, and the live approval-pending state now offers a direct retry action.
- [x] Built `cpp/build-release/amalgam_launcher_bedrock_auth.exe` as a separately named verification binary because the active `amalgam_launcher.exe` (PID 24232) still holds the canonical output open. The canonical link failure is only `LNK1104` from that lock; compilation succeeded and the alternate binary linked successfully.
- [x] Full native CTest passed 45/45. CLI preflights passed 3/3 (`--check-prereqs`, `--check-java`, `--check-official-launcher`).
- [x] Fresh host-valid visual evidence passed for Bedrock Coming Soon and the Microsoft approval-pending fallback at 1242x610 in `artifacts/audit-2026-09-22/launcher/bedrock-coming-soon-microsoft-approval-final-2-20260924T035229742Z-6b168a4b` (2/2). The Bedrock screenshot was manually inspected after correcting a capability-tile layout defect; no nested scroll surface is present.
- [ ] Replace the locked canonical executable after the owner closes the currently running launcher, then bind the final release hash and recapture the required evidence against `amalgam_launcher.exe`.
