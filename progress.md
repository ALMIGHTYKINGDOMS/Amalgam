# Amalgam Progress Log

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
