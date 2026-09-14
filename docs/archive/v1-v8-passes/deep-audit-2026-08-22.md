# Amalgam Launcher — Deep Project Audit

**Audit date:** 2026-08-22  
**Scope:** native launcher, launcher UI, in-game client UI, Java/NeoForge/Forge bridge staging, Bedrock add-on staging, provider catalog flows, packaging, and runtime smoke checks.

## Executive result

The project is internally coherent enough for a controlled beta, but it is not honestly an official public release yet. The native launcher build is healthy, the automated suite is green, the complete launcher route matrix renders, and the release archive passes structural and extracted-runtime validation.

The current handoff identity is **Amalgam Launcher 3.0.0-beta.1 (Beta v1)**. Normal startup is intentionally interruption-free: the beta welcome and crash-recovery/safe-mode dialogs are no longer shown automatically. Troubleshooting safe mode remains available explicitly through `--safe-mode`.

The remaining launch blockers are mostly environment or service gates rather than missing visual screens:

- Microsoft account authorization and a real Minecraft-owned account still need a live test.
- CurseForge needs a valid provider path: a real API key or an authenticated Amalgam proxy. The value previously pasted into chat is not a usable CurseForge API key.
- The temporary JDK 21 runtime available on this machine has no Java compiler, so a fresh Fabric/NeoForge source rebuild cannot be claimed from this environment. Staged bridge jars are present and the native bridge tests pass.
- The official Minecraft launcher is not installed/detected on this machine.
- Inno Setup Compiler is not installed, so the validated ZIP is ready but a rebuilt signed installer cannot be produced here.
- Clean-machine Java launch, Bedrock online/offline behavior, and signed-artifact verification remain external validation gates.

## What was audited

### Native and automated checks

- Rebuilt the changed native launcher target with MSVC/Ninja.
- Ran the complete CTest suite: **35/35 passed** in approximately 25 seconds.
- Refreshed all 23 deterministic launcher routes at 1536×1024 and 1280×720: `home`, `discover`, `library`, `profile`, `project`, `downloads`, `settings`, `account`, `servers`, `server-detail`, `server-console`, `cloud`, `bedrock`, `essentials`, `admin`, `java`, `backups`, `logs`, `config`, `theme`, `performance`, `social`, and `mods`.
- Package validation passed for `dist/amalgam-3.0.0-beta.1.zip`.
- Extracted-package runtime validation passed, including the 19 bridge jars, branding/art inventory, native artifacts, and Bedrock add-on package.
- The Discover featured grid now drops to three cards before the 1280px content canvas, preventing right-edge clipping while preserving four cards at full desktop width.

### Local runtime smoke checks

- `--doctor`: native bridge, 19 Java bridges, Java runtime discovery, Bedrock detection, Bedrock client package, and release integrity are ready. The only required readiness item reported is Microsoft account connection.
- `--check-java`: Java 17 and Java 21 are detected.
- `--check-official-launcher`: not detected on this machine.
- `--bedrock-info`: Bedrock data directory is detected.
- `--launch 1.21.1 fabric --dry-run`: completed successfully; cached client/assets/natives, Fabric API, bridge injection, and launch command assembly all verified without spawning the game.
- Live Modrinth search returned real catalog entries and image URLs.
- The all-provider search path currently returned Modrinth results only; CurseForge must be tested after a valid key/proxy session is available.

## Work completed in this audit pass

### Slow-path fixes

The largest verified UI hitch was render-thread filesystem work. The following are now cached, throttled, or moved behind a worker:

- Profile storage totals and per-world sizes no longer recursively scan the profile on every frame.
- World and screenshot enumeration uses short-lived indexes instead of rescanning every frame.
- Library world discovery is refreshed on a bounded interval.
- Quick search builds an in-memory index and searches that index; it no longer walks profiles/worlds on every keystroke/frame.
- Log filtering is revision-cached and only recomputes when the source or filter changes.
- Installed-mod directory enumeration is throttled.
- Catalog icon downloads are capped at four concurrent requests so a large result set cannot create an unbounded detached-thread burst.

### UI/UX/reference-board alignment

- Minecraft-themed catalog art now fills missing provider artwork instead of leaving empty or generic surfaces.
- Project detail pages use a branded Minecraft banner with a dark readability overlay while retaining the provider icon.
- Discover and project tabs have consistent active states and clearer hierarchy.
- Narrow filter rows stack safely instead of clipping or relying on a wide-window assumption.
- Empty states use a consistent compact Amalgam treatment instead of oversized prototype-looking placeholders.
- The in-game client rail uses the real Amalgam art, visible “AMALGAM / IN-GAME CLIENT” branding, and labeled navigation aligned to the supplied client reference board.
- The launcher reference art is now represented across home, discover, project detail, servers, essentials, account, performance, and profile surfaces.
- `supabase/.temp/` is now ignored so local Supabase connection material is less likely to enter a source package.
- Microsoft approval errors now explain that the device-code flow is waiting on the browser or Minecraft/Xbox approval, and recognized approval failures offer the official Minecraft Launcher fallback instead of trapping the player in a generic retry loop.
- When enabled in Accessibility settings, keyboard navigation now provides predictable Ctrl+1 through Ctrl+6 page shortcuts plus Ctrl+, for Settings, while leaving text-entry fields untouched.
- Beta and crash-recovery dialogs no longer interrupt normal startup; explicit `--safe-mode` remains available for recovery without changing or deleting user data.

## Performance diagnosis

### Confirmed causes

1. **Synchronous profile scans in read-only views.** This was the most important local cause of frame hitches when opening Library/profile detail and switching tabs.
2. **Per-frame quick-search discovery.** Searching while the index was being rebuilt could repeatedly touch profile/world data.
3. **Repeated installed-mod enumeration.** Mod Manager could repeatedly inspect the mods directory while the page was visible.
4. **Unbounded icon download fan-out.** Large catalog pages could create too many image workers at once.

These four paths are addressed in the current binary.

### Remaining expected waits

- First live provider search still waits on network latency, but it runs off the render thread.
- First-time catalog art still needs network fetch/decode, but it is cached and bounded.
- A cold, very large profile storage scan still takes time; it now reports a calculating state rather than freezing the page.
- Java discovery and first launch perform real filesystem/download work and need visible progress on a live machine.

## Remaining work, in priority order

### P0 — required before public release

1. Run Microsoft device-code/login with a real account after app approval and verify the token-to-Minecraft-services path.
2. Run one clean Java launch for each supported family: vanilla, Fabric, Forge, NeoForge, and Quilt where applicable.
3. Provide a valid CurseForge API key or deploy/authenticate the Amalgam CurseForge proxy, then verify search, project detail, file resolution, install, update, and import/export.
4. Install Inno Setup, rebuild the custom installer, and verify install/upgrade/uninstall on a clean Windows machine.
5. Code-sign the launcher, DLL, installer, and release archive; verify SmartScreen and signature metadata.
6. Validate Bedrock import/install and the supported online/offline flows on a real Minecraft for Windows installation. Keep Bedrock capability boundaries explicit; it is not the same injection model as Java.

The native release-candidate pass for all items that can be verified locally is complete. These six items are external acceptance gates, not tasks that can be truthfully marked complete without the corresponding Microsoft/provider/machine/install/signing state.

### P1 — required for AAAA polish

1. Run a real user-flow pass with mouse/keyboard at 100%, 125%, 150%, and 200% Windows scaling.
2. Validate first-run, offline, expired-session, provider outage, cancelled download, failed dependency, corrupted archive, and interrupted update states.
3. Validate all project detail tabs with live Modrinth and CurseForge projects, including missing gallery/changelog/version data.
4. Finish live backend validation for account, friends, messaging, Essentials relay, server transport, and profile synchronization.
5. Add a bounded first-party performance trace for page-open duration, provider request duration, icon queue depth, and background scan duration, with no secret or personal-content collection.
6. Complete translation coverage for all newly added strings and check text expansion in every compact layout.

### Intentional non-blocker

Amalgam Cloud remains intentionally gated as “Coming Soon” until the hosting backend is real. It should not be presented as an operational paid service before that backend exists.

## Release decision

**Current decision:** private beta / controlled tester release is reasonable after the Microsoft and provider checks. **Public official launch is not yet cleared.** The remaining work is measurable and is now concentrated in live credentials, clean-machine validation, signing, installer production, and the Java/Bedrock runtime matrix rather than another broad UI rewrite.
