# Amalgam Release Readiness Task Plan

**Goal:** Close the verified gaps from `findings.md`, then certify the launcher/client against real provider, install, launch, package, and clean-machine workflows.

**Current phase:** Phase 6 — Runtime and release certification

**Next Step:** Run a user-approved idle CPU/navigation comparison on the performance build, then certify or explicitly block the external provider/auth/clean-machine/signing gates.

## Phases

- [x] Phase 1 — Provider and account configuration readiness (repository checks and live Supabase health pass; active launcher config remains intentionally blank)
- [ ] Phase 2 — Version catalog, dependency resolution, and modpack installation (Modrinth passes; CurseForge is externally unconfigured)
- [x] Phase 3 — Version preflight and Java Edition route correction
- [ ] Phase 4 — Local server, Essentials, Downloads, and Bedrock runtime journeys (transport and Bedrock launch pass; gameplay and full server lifecycle remain)
- [x] Phase 5 — Scroll/layout regression and visual polish (shared sidebar wheel ownership fixed and rechecked; legitimate page scrolling preserved)
- [ ] Phase 6 — Release packaging, installer, clean-machine validation, and certification (package/installer pass; clean account and signing remain)

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
| Live focus helper timed out once while probing a window | 1 | Rechecked process lifetime and logs; no reproducible application crash was found |
| Supabase live verifier returned HTTP 401 without a publishable key | 1 | Recorded the missing runtime configuration as an external provider gate; did not print or embed credentials |
| New Java route assertion failed because the helper did not exist | 1 | Added the typed route helper and wired the Play sidebar mapping to the existing Java Manager surface |
| Direct local-server probe failed because its required `server.jar` fixture was absent | 1 | Re-ran the project’s official disposable echo-server fixture; transport, file boundary, console, and graceful-stop checks passed |
| Bedrock launch returned Windows error 2 with the previous AUMID | 1 | Queried the installed Start-menu registration, corrected the UWP AUMID to `!Game`, narrowed package detection, rebuilt, and retested successfully |
| Release package rejected the stable Bedrock package when a beta version was requested | 1 | Used the project’s aligned `1.0.0` release identity for packaging; no release artifact was overwritten (output staged under `dist/plan-2026-08-31`) |
| Compact-window wheel input moved the fixed sidebar instead of the page surface | 1 | Added `NoScrollWithMouse` to the shared sidebar child, rebuilt, reran the compact interaction check, and recaptured the route matrix |
| Silent Inno uninstall stalled on the custom user-data prompt | 1 | Guarded the destructive prompt with `UninstallSilent()`, rebuilt the installer, and verified a fresh silent uninstall exits 0 and removes installed binaries while preserving user-created files |
| Release smoke script targeted an obsolete package path | 1 | Added the `-PackageDir` parameter and reran prerequisite and Java smoke checks against the current fixed-sidebar package |

## Completion gate

The plan is complete only when all six phases are checked, CTest is green, the Bedrock validator is green, a real content install launches, a real local server journey passes, Microsoft/Supabase/CurseForge checks are either live and green or explicitly disabled with a user-facing explanation, and the packaged installer passes clean-machine validation.
