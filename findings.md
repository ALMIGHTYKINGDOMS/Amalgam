# Amalgam Audit Findings — 2026-08-31

This file records the evidence from the latest build, automated test run, live desktop pass, and package checks. It is audit data, not implementation instructions.

## Verified passes

- Fresh MSVC Release build completed successfully in `cpp/build-vs`.
- CTest completed with 36/36 tests passing.
- `bedrock/AmalgamBedrockClient/tools/validate.js` completed successfully.
- Live navigation and rendering were checked for Home, Discover, Modrinth project details, Library, Downloads, Essentials, Servers, Settings, and Bedrock.
- Modrinth discovery returned real featured projects, mods, detail content, changelogs, and version releases.
- A real Modrinth mod install completed into a disposable profile with a verified jar.
- A real Modrinth modpack install completed into a disposable isolated profile with 48 mods and 3 resource packs; the profile metadata, loader, and selected Minecraft version were verified.
- A bounded Java Forge launch dry-run resolved the loader, Java 17, client, natives, assets, native DLL injection, and bridge mod without spawning gameplay.
- The project server transport fixture passed against the fresh launcher: file operations, path traversal rejection, console stdin/stdout, and graceful stop.
- Live Supabase health passed with the supplied publishable key: Auth endpoint and protected CurseForge function behavior were correct.
- Bedrock Overview, Profiles, Worlds, Backups, and Add-ons tabs rendered with their empty states and controls.
- Login and registration surfaces opened and closed through their own controls without requiring a launcher restart.
- Profile creation, server creation, and Bedrock profile wizards opened, rendered, and cancelled correctly.
- The recovery dialog close-state defect was fixed in `cpp/launcher/src/ui.cpp`, rebuilt, and retested.
- A clean Alt+F4 shutdown removed `.amalgam_running`; a subsequent launch did not show a false recovery dialog.
- Bedrock package detection was narrowed to the Bedrock UWP package and launch activation was corrected to the current Windows AUMID (`!Game`); `--bedrock-launch` returned success and the test process exited cleanly.
- A strict fresh Release build completed in `cpp/build-release`; the archive, extracted runtime, Inno installer, and installed-package prerequisite/Home smoke checks passed.
- The minimum-size visual route matrix completed at 960x600 for 22 launcher routes; every route rendered and exited successfully, and the review found only legitimate long-content scrolling.
- Cached launch validation completed successfully for Fabric, Quilt, NeoForge, and Forge across the supported 1.12.2, 1.18.2, 1.19.2, 1.20.1, and 1.21.8 cases.
- A clean native real-user QA session completed live navigation through Home, Discover, Library, Downloads, Essentials, Servers, and Settings, including global search and wheel checks; the session exited cleanly with no reproducible clipping or crash.
- The compact-window focused check reproduced and fixed a shared shell defect where the sidebar accepted page-wheel input; the rebuilt executable keeps the logo/navigation fixed, while a fitting Settings section does not move unnecessarily.
- The updated Inno installer was tested from a fresh disposable directory: silent install, packaged prerequisite check, installed Home snapshot, and silent uninstall all passed; user-created config/log/snapshot files were intentionally preserved.
- Release smoke tooling was corrected to accept an explicit package directory; the current fixed-sidebar package passed both prerequisite and Java smoke checks through that path.
- A second disposable installer run covered same-version repair/upgrade: the first-launch config hash remained unchanged, post-upgrade prerequisites passed, and silent uninstall removed binaries while preserving the user-created config.
- The desktop render loop was frame-paced at approximately 60 FPS; the previous one-millisecond delay left rendering effectively uncapped and allowed the shell to compete with Minecraft for CPU/GPU time.
- Recurring profile content, world-count, screenshot-count, Library-world, and Screenshots-gallery directory scans were moved to background workers with generation checks so stale results cannot overwrite a newly selected profile.
- The updated Visual Studio Release build passed all 36 CTest cases, clean extracted-package runtime validation, and packaged launcher prerequisite/Java smoke checks without opening the launcher window.

## Findings still open or unverified

1. The current `cpp/build-vs/launcher.json` has no active CurseForge runtime credential or proxy configuration, so CurseForge discovery could not be verified in this build.
2. The current runtime has no Microsoft client ID, so Microsoft/Xbox authentication could not be exercised end to end.
3. The current runtime has no active Supabase configuration, so launcher-to-website account persistence could not be exercised end to end.
4. CurseForge discovery and installation remain unverified because the active runtime has no CurseForge key or approved proxy configuration.
5. A real Modrinth modpack was imported and validated, but a playable authenticated Minecraft launch was not completed in this pass.
6. The local server transport journey passed, but a full Minecraft server create/download/start/restart journey and long-running server health check remain unverified.
7. Bedrock detection and launch now pass, but companion-client behavior and online/offline gameplay remain unverified; Microsoft account approval is still external.
8. Supabase live endpoints pass, but the installed launcher template is intentionally unconfigured, so launcher↔website account persistence remains unverified in this runtime.
9. Disposable install/launch/repair-upgrade/uninstall passed after the silent-uninstall fix, but a separate Windows test-account rollback run and public signing certificate validation remain unverified.
10. The demonstrated compact scroll-ownership defect is fixed: the sidebar no longer captures page-wheel input. The Settings section remains stationary when its content fits; further minimum/maximum DPI coverage is optional release hardening, not a known defect.
11. One desktop focus-helper timeout occurred during probing; repeated process checks and runtime logs did not show a reproducible product crash.
12. This performance pass was validated headlessly because the user was actively playing a game; no new foreground visual session was run. A follow-up idle CPU comparison and screenshot traversal should be run when the user is free to test the installed build.

## Constraints

- Provider credentials and service keys must stay outside source control and outside generated code.
- The release must preserve the existing 19 Java bridge artifacts and Bedrock package validation rules.
- No custom Minecraft packets may be introduced.
- A feature is not considered complete until its runtime path, error path, and clean shutdown path are tested.
