# Amalgam Release Decision — 2026-08-31

## Decision

**Private QA package: CONDITIONAL GO.** The packaged native launcher, Bedrock package, bridges, artwork, installer, and repository-controlled runtime paths are ready for controlled testing.

**Official public release: NO-GO for now.** The remaining blockers are external or require a clean test environment that was not available in this pass.

## Evidence-backed status

| Area | Status | Evidence or blocker |
|---|---|---|
| Native launcher/client | PASS | Fresh MSVC Release build and 36/36 CTest pass |
| Java bridges and cached routes | PASS | Seven-case Fabric/Quilt/NeoForge/Forge matrix passed |
| Modrinth | PASS | Live discovery, mod install, and isolated modpack import passed |
| CurseForge | BLOCKED | No active runtime key or approved proxy configured |
| Supabase endpoints | PASS | Auth and protected-route health passed with publishable-key health check |
| Launcher ↔ website account persistence | NOT TESTED | Installed launcher template is intentionally unconfigured |
| Microsoft/Xbox authentication | BLOCKED | Approval/client configuration is external to this build |
| Java playable launch | NOT TESTED | Preflight/dry-run passed; authenticated gameplay was not exercised |
| Local server transport | PASS | File safety, console transport, and graceful-stop fixture passed |
| Full Minecraft server lifecycle | NOT TESTED | Real create/download/start/restart/long-running health was not completed |
| Bedrock detection and activation | PASS | Current UWP package detected; `!Game` activation returned success |
| Bedrock companion/gameplay | NOT TESTED | Online/offline gameplay and addon behavior need an installed test game |
| UI route rendering | PASS | 22 routes rendered at 960x600 after the shared-sidebar scroll fix; no new accidental clipping demonstrated |
| Installer/package | PASS | Package, runtime, Inno compile, isolated install, prerequisite, Java smoke, Home smoke, silent uninstall, and secret scan passed |
| Responsiveness pass | PASS | Render loop is paced at ~60 FPS; recurring profile and gallery directory scans run off the render thread; 36/36 tests and headless smoke pass |
| Clean-account lifecycle | PARTIAL | Disposable install/launch/repair-upgrade/uninstall passed and config was preserved; no separate Windows test account was available for rollback |
| Code signing | BLOCKED | No release certificate is installed; signing verification is advisory only |

Release artifacts:

- `dist/plan-2026-08-31-fixed-sidebar/amalgam-1.0.0.zip`
- `dist/plan-2026-08-31-fixed-sidebar/installer/AmalgamLauncher-1.0.0-Setup.exe`
- `dist/plan-2026-08-31-performance/amalgam-1.0.0.zip`
- `dist/plan-2026-08-31-performance/installer/AmalgamLauncher-1.0.0-Setup.exe`

No provider secret or account password is included in this document or the artifacts.
