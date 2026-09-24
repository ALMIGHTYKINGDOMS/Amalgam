# Amalgam Release Readiness Task Plan

**Goal:** Complete the repository-resolvable Amalgam V2 scope with risk-first stabilization, secure integrations, production-quality UI/UX, exhaustive deterministic visual evidence, and honest release certification.

**Current phase:** Phase 19 — official 1.0.0 launch-readiness certification complete; candidate staged with external-owner gates remaining

**Next Step:** An owner with Microsoft production approval, a trusted code-signing certificate, live deployment credentials, and authorized release-upload access must complete the remaining external gates before public promotion. The repository candidate and local certification work are complete.

## Phases

- [x] Phase 1 — Provider and account configuration readiness (repository checks and live Supabase health pass; active launcher config remains intentionally blank)
- [ ] Phase 2 — Version catalog, dependency resolution, and modpack installation (Modrinth passes; CurseForge is externally unconfigured)
- [x] Phase 3 — Version preflight and Java Edition route correction
- [ ] Phase 4 — Local server, Essentials, Downloads, and Bedrock runtime journeys (transport and Bedrock launch pass; gameplay and full server lifecycle remain)
- [x] Phase 5 — Scroll/layout regression and visual polish (shared sidebar wheel ownership fixed and rechecked; legitimate page scrolling preserved)
- [ ] Phase 6 — Release packaging, installer, clean-machine validation, and certification (package/installer pass; clean account and signing remain)
- [x] Phase 7 — September full-project audit and refinement design (current build/tests, security/release tooling, launcher visuals, public-site QA, and working-tree provenance audited)
- [ ] Phase 8 — P1 stabilization slice (server-name/path safety, Forge/NeoForge launch quoting, provisioning error containment, artifact-integrity metadata, and regression tests)
- [ ] Phase 9 — Security and release-gate hardening (multi-root secret scan, effective Supabase function privilege/search-path audit, RLS tests, and working-tree classification)
- [ ] Phase 10 — Product-truth and visual polish (profile Content empty/search state, README/capability claims, website-source alignment, and live-site accessibility/layout stability where source is available)
- [ ] Phase 11 — Full verification and certification (clean native/Java/Bedrock/Node/Supabase checks, real provider/server journeys, packaging/installer, live interaction, auth persistence, and explicit external blockers)
- [ ] Phase 12 — Exhaustive launcher visual evidence and AAA-quality polish (fresh deterministic captures for all reachable pages, tabs, populated/empty/loading/error states, wizards, dialogs, overlays, responsive sizes, and scroll positions; fix and recapture demonstrated defects)
- [ ] Phase 13 — Risk-first V2 stabilization (eliminate repository-resolvable data-loss, live-provider fixture, destructive-action, authorization-scope, and stale-session hazards; add targeted regression coverage)
- [ ] Phase 14 — Full-system completion audit and integration repair (Java/loader/runtime, updater/recovery, account/Supabase/Essentials, Bedrock, servers, downloads, installer/package, website truth, performance, and dormant/unreachable code)
- [ ] Phase 15 — Meaningful-surface UI/UX completion (replace incomplete/blank/error-prone interaction states; extend deterministic local fixtures to every meaningful dialog/menu/wizard/state; complete responsive and accessibility polish)
- [ ] Phase 16 — Whole-project verification and release decision (fresh native/Java/Bedrock/Node/Supabase-source builds/tests, package checks, live integrations where credentials allow, full visual matrices, secret/security scans, and explicit external blockers)
- [ ] Phase 17 — Exhaustive interaction-fixture safety and coverage expansion (make fixture actions fail closed; add local-only deterministic evidence for every reachable modal, menu, destructive/recovery state, and secondary panel; rebuild, test, visually inspect, and recapture the complete expanded matrix)
- [ ] Phase 18 — Controlled local release completion and certification (trace current binary/evidence/package provenance, test a disposable installed candidate, close any newly reproduced repository defects, and isolate genuine external certification gates)
- [ ] Phase 19 — Official 1.0.0 launch-readiness certification (clean build, current UI evidence, Java-only installed-candidate verification, package/installer/secret/hash gates, truthful support matrix, and explicit external-owner launch decision)

## 2026-09-23 — Official launch-readiness gate

- [x] Rebuild the current source after the configuration entitlement/scroll and server fixture wrapping fixes; run the full current native and tooling suites (45/45 native tests passed).
- [x] Review the remaining non-Bedrock visual evidence states: the documented route matrix and theme shells are covered; tooltip hover, open combo, toast, and explicit DPI microstates remain a documented P2 evidence follow-up rather than a missing page/tab/wizard or a reproduced launch defect. Fixture actions remain inert and Bedrock routes stay excluded from final runtime capture.
- [x] Capture and review the complete Java-only matrix at the honest host-valid sizes (`1256x624` and `960x600` requests; actual client area recorded), then invalidate any evidence affected by later fixes (733/733 captures passed; 733/733 reviewed images valid).
- [x] Re-stage all 19 Java bridges and the static Bedrock package triplet from current source/build outputs; create a unique immutable `1.0.0` candidate without overwriting older artifacts.
- [x] Validate the candidate ZIP, installer inputs, secret scan, hashes, SBOM/component manifest, Java preflight, and official-launcher guard. Do not run Bedrock detection/launch/import or disturb the active Java session.
- [x] Run disposable installed-copy checks: first run, prerequisites, config/session preservation, Java-only profile/handoff preflight, restart, repair/upgrade/uninstall safety where feasible.
- [x] Close repository-resolvable P0/P1 defects; classify Microsoft approval, trusted code-signing, production provider/backend credentials, and any unavailable clean-machine/real-game proof as explicit external owner gates.
- [ ] Do not publish, promote the website, switch the public updater channel, or claim Microsoft-authenticated Java launch until those external gates are actually satisfied.

## Responsiveness pass

- [x] Cap the desktop render loop at approximately 60 FPS.
- [x] Move recurring profile content/count and gallery directory scans off the render thread.
- [x] Guard asynchronous cache updates with generations so profile switches and invalidations cannot accept stale results.
- [x] Rebuild, run 36/36 tests, validate the extracted package, and run headless package smoke checks.
- [ ] Run live idle CPU and interaction measurements when the user is not actively playing a game.

## Errors Encountered

| Error | Attempt | Resolution |
|---|---:|---|
| JNI headers were missing from the temporary JDK 21 runtime | 1 | Switched the C++ build environment to the installed full JDK 17 containing JNI headers |
| Windows `max` macro conflicted with `std::max` in `net.cpp` | 1 | Changed the call to the parenthesized Windows-safe form and rebuilt successfully |
| Recovery dialog restored its open state after the primary button click | 1 | Cleared both local and shared dialog state before the frame-end synchronization, then rebuilt and retested |
| Fresh September build wrapper exited 1 with no output because the nested `cmd` quoting was malformed | 1 | Verified the Visual Studio environment script exists; retry with `cmd.exe /d /s /c` quoting that preserves the batch-call environment |
| Direct `ctest` invocation was not on the default PowerShell PATH | 1 | Run CTest from the same Visual Studio developer environment used by the successful CMake build |
| Website package manifest was not at `website/package.json` during the ecosystem inventory | 1 | Treat the website as a nested/multi-project tree and locate manifests with `rg --files` before running its checks |
| `agent-browser` was not installed as a global command | 1 | Use the skill-supported `npx agent-browser` entry point and load its version-matched core/dogfood instructions before browser QA |
| PowerShell consumed the unquoted `@e100` browser element ref, so the mobile-menu click received no selector | 1 | Quote browser refs in PowerShell (`'@e100'`) before the interaction retest |
| Snapshot commands returned 0 but produced no files at the requested relative artifact paths | 1 | Logs proved rendering succeeded; absolute output paths persist correctly, so all audit captures now use resolved absolute paths |
| Live focus helper timed out once while probing a window | 1 | Rechecked process lifetime and logs; no reproducible application crash was found |
| Supabase live verifier returned HTTP 401 without a publishable key | 1 | Recorded the missing runtime configuration as an external provider gate; did not print or embed credentials |
| New Java route assertion failed because the helper did not exist | 1 | Added the typed route helper and wired the Play sidebar mapping to the existing Java Manager surface |
| Direct local-server probe failed because its required `server.jar` fixture was absent | 1 | Re-ran the project’s official disposable echo-server fixture; transport, file boundary, console, and graceful-stop checks passed |
| Bedrock launch returned Windows error 2 with the previous AUMID | 1 | Queried the installed Start-menu registration, corrected the UWP AUMID to `!Game`, narrowed package detection, rebuilt, and retested successfully |
| Release package rejected the stable Bedrock package when a beta version was requested | 1 | Used the project’s aligned `1.0.0` release identity for packaging; no release artifact was overwritten (output staged under `dist/plan-2026-08-31`) |
| Compact-window wheel input moved the fixed sidebar instead of the page surface | 1 | Added `NoScrollWithMouse` to the shared sidebar child, rebuilt, reran the compact interaction check, and recaptured the route matrix |
| Silent Inno uninstall stalled on the custom user-data prompt | 1 | Guarded the destructive prompt with `UninstallSilent()`, rebuilt the installer, and verified a fresh silent uninstall exits 0 and removes installed binaries while preserving user-created files |
| Release smoke script targeted an obsolete package path | 1 | Added the `-PackageDir` parameter and reran prerequisite and Java smoke checks against the current fixed-sidebar package |
| First full visual-matrix runner depended on a PowerShell `Get-FileHash` module that was unavailable in its standalone process | 1 | Replaced that runner dependency with an in-process .NET SHA-256 helper before any successful evidence run |
| Full visual runner discovered Cloud fixture would call live provider APIs | 1 | Stopped and rejected the incomplete run before it reached Cloud; added a local-only Cloud facade and will restart from a new immutable directory after the broader safety gate |
| Direct `Start-Process` smoke command split an output path containing spaces | 1 | Pass the snapshot arguments as one explicitly quoted command-line string, matching the hardened capture runner |

## Current visual-evidence rules

- Fixture captures must be isolated from real accounts, launcher configuration, Bedrock installations, cloud providers, and user files. They demonstrate composition and layout only; they do not substitute for live feature verification.
- Every capture must be traceable through the generated JSON/Markdown ledger, including its exact route, scroll position, dimensions, image hash, and output path.
- A surface is not counted as fully reviewed merely because it has a top screenshot: scrollable surfaces require top, middle, and/or bottom evidence as defined in the matrix, and later visual fixes require recapture of affected routes.
- Account and Bedrock fixtures must identify their representative data in the rendered UI and must not hydrate live sessions or mutate real state.
- The current evidence host is physically 1280×720. Its host-valid 1256×624 and 960×600 requests must be reported with their actual client dimensions; clamped images must never be presented as 1600×900 or 1366×768 evidence.

## Completion gate

The plan is complete only when all twelve phases are checked, every P1 above has a regression test, CTest/Java/Bedrock/Node/Supabase checks are green, a real content install and local-server journey pass, public claims match the release artifact, Microsoft/Supabase/CurseForge checks are either live and green or explicitly disabled with a user-facing explanation, the captured launcher matrix has a clean hash-verified ledger with every scoped route represented, and the packaged installer passes clean-machine validation.

## Expanded V2 completion gate

The V2 completion request adds these non-negotiable constraints: every repository-resolvable P0/P1 must be fixed rather than merely documented; high-impact user actions must have truthful confirmations and final authorization checks; fixture evidence must never contact providers or leak/modify reviewer state; every meaningful active launcher dialog, menu, wizard step, and long-page state must receive local deterministic evidence; and all remaining items in the final report must be genuine external prerequisites rather than unfixed code.

## 2026-09-23 stabilization checkpoint

- [x] Isolate the native Microsoft-auth regression test from the real local account path; its test-only `LOCALAPPDATA` sandbox passed without creating a normal user account file.
- [x] Make runtime-agent telemetry distinguish unavailable values from measured zero; expanded runtime-agent regression coverage passed 38/38 and the local Supabase source contract verifier passed (25 migrations, 24 Edge Functions). This does not prove a live database migration was applied.
- [x] Harden update-manifest production defaults: signed manifests, positive bounded payload sizes, and payload size/hash/signature verification are now covered by 12 Node regression tests.
- [x] Run the current source/package-input secret scan without printing credential material: scanner self-tests passed 2/2; 522 scoped text files across launcher, Bedrock, bridge, Supabase, tooling, and installer inputs produced 0 secret-category matches.
- [x] Complete and build-test the account-action coordinator: explicit local-scope sign-out, transactional protected-local-session persistence, separate Minecraft disconnect, target revalidation, and confirmation routes.
- [x] Complete and build-test direct-launch Java/native-layout hardening, then run the full native regression suite once source changes settle.
- [x] Complete release-gate ordering and final-artifact integrity checks. A publisher-confirmed payload endpoint, signing identity, and signing private-key authority remain external prerequisites.
- [ ] Repair the visual capture runner and collect a new immutable evidence set. Existing captures are historical until the current binary and a non-clamped viewport environment produce a hash-verified ledger.

## 2026-09-23 active stabilization / evidence checkpoint

- [x] Repair the local-server remove-entry confirmation so it is reachable from both list and detail views, preserves the saved entry when persistence fails, and truthfully states that server folders/files are left untouched.
- [x] Repair local-server file deletion feedback so filesystem failure or a vanished target leaves the confirmation open with a precise, retryable outcome instead of claiming completion.
- [x] Add deterministic, non-network account/authentication fixture composition for account creation, email verification, password recovery, and every Microsoft device-code outcome.
- [x] Add deterministic, non-network server destructive-action fixture composition for file delete, backup restore, and saved-entry removal.
- [x] Make the binary/manifest capture boundary fail closed. The binary now exports its accepted fixture inventory; the runner rejects schema/count/set/hash mismatches before it creates an artifact directory or PNG.
- [ ] Batch-build the above with the in-progress account/authentication and Essentials/social nonblocking request lanes, then rerun the focused and full native suites.
- [ ] Re-run the inventory gate against the fresh executable. Current source declares 203 accepted fixture routes, 277 canonical visual cases, and 695 planned responsive captures; the old executable intentionally remains rejected until rebuilt.
- [ ] Extend fixture-backed visual proof to the remaining meaningful destructive/recovery dialogs (Bedrock worlds/backups/add-ons, mod removal/details, cache cleanup, profile recovery, managed Java, and administrative confirmations).

## 2026-09-23 exhaustive interaction-fixture expansion checkpoint

- [x] Rebuilt and re-tested the latest user-facing P2 fixes before the evidence expansion: the native Release tree reported `ninja: no work to do` after its rebuild and CTest passed in two bounded batches (22/22 and 22/22; 44/44 total). The current manifest parses and its update-manifest suite remains green (12/12); runtime-agent remains green (38/38); local Supabase source verification remains green (25 migrations, 24 Edge Functions). These are source/build/test checkpoints, not a replacement for the pending full visual proof.
- [x] Repaired and static-validated the capture harness: schema v3 preserves every first artifact and fresh isolated retry attempt for an exit-0 exact-1x1 artifact or material viewport clamp. A focused normal proof passed 4/4 at the two host-valid requests; an intentional 1600x900 host-clamp proof retained two rejected 1242x610 attempts and failed as required. The first partial broad run remains preserved as diagnosis only, never certification.
- [x] Ran a fresh registry/manifest active-surface audit. The current 221 registered tokens and 296 cases have exact parity, but parity alone is insufficient for the user's requested every-page/tab/wizard/modal/menu standard.
- [ ] Eliminate the newly discovered fixture-safety P1 before adding routes: existing Server remove-entry and file-delete fixture overlays render production confirmation handlers that could otherwise persist/remove data if a reviewer clicks them; backup-restore currently reports inertness but remains enabled. All fixture-mode mutation controls and action entry points must fail closed, and their capture copy must make that state explicit.
- [ ] Add missing active modal/menu fixture routes in risk order: profile restore/delete/options confirmations and errors; Mod Manager recovery/details/dependency views; Server restore/remove errors and file preview/context; typed Admin action confirmation; Essentials friend profile/context; cache cleanup; profile/library/version/creator-update/managed-Java/settings dialogs; then every remaining reachable overflow/context/combo/menu surface identified by the inventory.
- [ ] Build, run full CTest plus fixture-inventory validation, run focused visual proofs for every new route at both host-valid tiers, then run one expanded immutable all-case × two-tier capture ledger and inspect every generated contact sheet. Do not claim global visual completion before that ledger is clean.

## 2026-09-23 expanded fixture-integration checkpoint

- [x] Fail-closed the Server fixture action boundary at both control and operation-entry layers. File deletion, saved-entry removal, and backup restore are disabled in fixture mode and return before filesystem, persistence, or server-service calls.
- [x] Added 40 deterministic top-level local-only launcher routes plus one real nested-scroll checkpoint: profile restore/delete/options/version/creator-update and actions-menu states; library group and move-to-group actions; project install confirmations; screenshot and mod overflow menus; friend profile states; cache cleanup; typed Admin actions; managed Java dialogs; Server failure/preview states; and the Admin password dialog.
- [x] Updated the canonical matrix to 261 exact fixture tokens and 337 case records. Static source/manifest comparison reports zero missing or dangling routes; every new top-level route is selected by both host-valid high-risk tiers.
- [ ] Finish the remaining read-only interaction-surface audit, then compile the integrated source once, rerun the split native suite, validate the compiled inventory, and visually inspect focused captures before starting the complete 337-case × two-host-valid-tier ledger.

## 2026-09-23 final local UI/evidence checkpoint

- [x] Completed the final local fixture-quality pass. The current Release launcher build succeeded; CTest passed in two split runs (22/22 and 22/22); the update-manifest Node suite passed 12/12.
- [x] Captured one immutable full matrix at the two host-valid tiers: 792/792 passed, 0 failed, 0 blocked, with no transient retry/recovery. The ledger verified binary, manifest, fixture-inventory, case-selection, and configuration-isolation provenance.
- [x] Generated 257 review contact sheets and validated all 792 images: no missing, invalid, dimension-mismatched, hash-mismatched, or provenance-invalid images. Direct review covered high-risk Server, Account, Bedrock, Performance, Essentials, and Cloud states at the compact tier.
- [x] Closed the last fixture-modal clipping issue by making dynamic dialog detail scroll independently while keeping recovery notice/action rows visible. The same fixed-footer layout was applied to the real join dialog.
- [ ] Start the next separate release-completion phase from docs/superpowers/plans/2026-09-23-release-completion-and-controlled-certification.md. The official website and live Minecraft network remain explicitly out of scope.

## 2026-09-23 release-completion audit checkpoint

- [x] Reconciled the current visual provenance: final capture ledger recorded 792/792 passed and its launcher SHA-256 (01e31dca30def65606e317cc5199cf56b5ed8cac471b888eca45ce14933956a0) still matches cpp/build-release/amalgam_launcher.exe. The current manifest hash also matches its captured copy.
- [x] Established that the existing stage, ZIP, and installer from 2026-09-15 are stale: their staged launcher SHA-256 is 077a7bbf3889b2a41109ef5a770dc6676d4054466f6dbe5abbb0ea871b3194dd, not the final visual-certified binary.
- [x] Rebuilt the documented Release target (Ninja reported no work needed) and ran fresh native CTest batches: 22/22 plus 22/22 passed. Fresh Node/source checks passed: update-manifest 12/12, runtime-agent 38/38, Supabase source verification (25 migrations / 24 Edge Functions), and Bedrock test/validation (4/4 plus validator).
- [ ] Repair P1: updater extracts a complete release package but currently only replaces/rolls back the launcher executable. The staged payload must be validated and applied as a complete transactional component set before an update feed can be enabled.
- [ ] Repair P1: installer-style local-server provisioning must validate a runnable target before reporting Ready.
- [ ] Repair P2: diagnostics must stop checking for a nonexistent updater.exe and must not call Modrinth connectivity an Essentials/Supabase health check.

## 2026-09-23 — Remaining work map (actively executing)

1. Finish and review the path-safe full-package updater transaction; then compile every active server, diagnostics, updater, desktop-backend-key, and package-tool repair together.
2. Run focused regressions followed by the prescribed full 44-test native suite and local Node/Bedrock/Supabase source gates. Re-capture affected local UI routes after any UI-visible change.
3. Rebuild or deliberately re-stage all 19 Java bridges and regenerate the Bedrock add-on/metadata triplet from the exact approved candidate identity; validate component digests, names, freshness, and package inputs.
4. Create a brand-new local candidate only beneath a unique artifact path; run package, installer, secret/debug, inventory, direct-executable, restart/recovery, and disposable uninstall/reinstall checks without touching ordinary data or existing `dist` artifacts.
5. Close any reproduced repository defect through a minimal regression/repair/retest loop. Only after those gates are green, report the remaining genuinely external certification prerequisites such as signing authority and controlled clean-machine/entitled-game testing.

Current hard gates: the EXE-only updater transaction; unbuilt server/diagnostics repairs; stale bridge staging; source-stale Bedrock staging; a desktop legacy service-key acceptance path; and a Bedrock metadata digest verification gap. The official site and live Minecraft network remain out of scope.

## 2026-09-23 — Java-only user-authorized test boundary

- The user has authorized use of their already signed-in official Minecraft Launcher profile for a Java Edition handoff/launch verification once the current candidate is built.
- A Java Minecraft session is currently active. Do not focus, close, alter, or otherwise disrupt it. First use Amalgam preflight/profile-handoff proof; only start a new Java session after the active one is closed or the user explicitly asks to replace it.
- **Bedrock is excluded:** do not launch, detect, import, activate, or test Minecraft for Windows, Bedrock worlds, or add-ons because the user reports it can crash this PC. Static package/integrity checks may remain in the record but are not a runtime result.

## 2026-09-23 — Integrated repair verification complete

- [x] Compile the full current C++ Release source after updater/server/diagnostics/desktop-key integration.
- [x] Run the prescribed native CTest halves: 22/22 passed for tests 1–22 and 22/22 passed for tests 23–44.
- [x] Re-run local update-manifest (12/12), runtime-agent (38/38), Supabase source (25 migrations/24 Edge Functions), secret scan (0 source matches across 522 scoped files), and package-script parser checks.
- [ ] Rebuild/re-stage package inputs in controlled local toolchains: all 19 bridge JARs plus the Bedrock archive/metadata triplet. No Bedrock runtime activity is permitted.
- [ ] Stage and validate a unique local `1.0.0` candidate only after those component inputs are fresh and mutually consistent.

## 2026-09-23 — Latest controlled certification pass

- [x] Rebuilt the current Release launcher after the final UI fixture/polish changes; the full native CTest suite passed 45/45.
- [x] Refreshed the complete Java-only visual matrix at the two host-valid client tiers. Four immutable ledgers contain 751/751 passed captures (190 + 186 + 190 + 185), all tied to launcher SHA-256 `E454EF0A87EFD9C180D40DF81A8E429C176404CDF8AFC277C236F06E8B957DAE`; the four review galleries report zero integrity-attention images. Bedrock runtime captures remain excluded by user instruction.
- [x] Added and verified deterministic transient UI evidence for the open five-theme selector, light/midnight/solarized/Dracula shells, success/error/stacked toasts, and navigation tooltip states. The open-picker review reports 2/2 valid captures with zero integrity attention.
- [x] Re-ran supporting gates: runtime-agent 38/38, update-manifest 12/12, Supabase source verification (25 migrations / 24 Edge Functions), Bedrock static truth 4/4 plus validator, and source secret scan with no unexpected HIGH findings. The candidate package scan reports 0 secret-category matches across 29 text files.
- [x] Rebuilt all package inputs into a unique local candidate at `artifacts/release-candidate-2026-09-23/rc-20260923T231319050Z`; ZIP/package validation, clean-extracted runtime validation, installer-input QA, Inno Setup compilation, component-manifest binding, CLI smoke checks, and an isolated Java official-handoff probe passed.
- [ ] Obtain trusted Windows signing/timestamping, Microsoft production approval, publisher-controlled update hosting/signature, live Supabase/provider deployment, and a real authenticated Java handoff after the active Java session is closed or the user explicitly authorizes replacement. Do not publish or promote until those external gates are satisfied.
- [ ] Do not run Bedrock runtime validation; retain only static package/hash/metadata evidence.

## 2026-09-23 — Live integration and GitHub handoff

- [x] Apply and verify the live Supabase RPC privilege-hardening migration for user-bound editor jobs, service-only credit grants, and the trigger helper.
- [x] Update the live Supabase verifier for representative table endpoints and confirm Auth plus protected CurseForge behavior without exposing provider secrets.
- [x] Build and validate the configured `1.0.0` release ZIP and installer with the real public Microsoft/Supabase/website configuration; run the complete 45-test native suite.
- [x] Add repository hygiene rules for local QA/build debris and run a scoped secret scan with no unexpected HIGH findings.
- [ ] Commit and push the reviewed source snapshot to the empty `ALMIGHTYKINGDOMS/Amalgam` repository after staged-diff review. Do not force-push or publish release artifacts.
- [ ] Close the remaining owner-controlled gates: Microsoft approval/entitled Java handoff, authenticated CurseForge verification, trusted signing, update hosting, and clean-machine release certification.

## 2026-09-23 — Two-pass audit closure

- [x] Perform an independent source/runtime/security/release audit while the repository identity is pending; no live-session or Bedrock-runtime activity was used.
- [x] Correct the stale `play.amalgam-network.com:9564` default to the canonical `play.amalgam-network.com` address supplied by the owner.
- [x] Add and pass a regression test covering the canonical Amalgam Network name and address.
- [x] Repeat the audit after rebuilding: native 45/45, runtime-agent 38/38, manifest 12/12, scanner 2/2, Bedrock static 4/4, production schema 66/66, live Supabase verifier PASS, package/runtime validation PASS, candidate scan 0 matches, and visual registry parity PASS.

## 2026-09-24 — Final deep-audit closure

- [x] Rebuilt and bound the latest verification binary after the final responsive modal polish; the active launcher remains untouched because it holds the canonical output open.
- [x] Re-ran the complete native suite (45/45), launcher preflights (3/3), runtime-agent (38/38), Bedrock static truth/validator (4/4 plus validator), update-manifest (12/12), secret scanner (2/2 plus scoped 287-file scan), Supabase source (26 migrations / 24 Edge Functions), and production schema (66/66).
- [x] Recaptured and manually inspected the affected Bedrock Coming Soon and Microsoft approval-pending routes at both host-valid responsive tiers; 8/8 captures passed with no nested/horizontal-scroll or modal-spacing defect.
- [x] Completed a read-only live Supabase audit and classified advisor findings as intentional/non-blocking or owner-configured; no unsafe live schema mutation was made during this final pass.
- [x] Final repository-resolvable polish is complete for this audit. The remaining checklist is external certification/owner work only: close the running launcher and bind the canonical hash, Microsoft/Xbox approval, trusted signing/timestamping, update hosting/signature, live provider verification, Supabase leaked-password protection, and clean-machine/real Java handoff.
