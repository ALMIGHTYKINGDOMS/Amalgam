# Amalgam Launcher 3.0.0-beta.1 — Official Launch Readiness

Date: 2026-08-22  
Channel: beta  
Decision: **Local release candidate complete; public launch remains NO-GO until signing and owner-authenticated game validation are closed.**

## Realistic status

These percentages are weighted product-readiness estimates, not line-count or
file-count percentages.

| Scope | Status | Why |
|---|---:|---|
| Feature/code milestone | **98%** | The launcher, provider flows, isolated profiles, downloads, local servers, Bedrock management, Essentials surfaces, native client, bridges, recovery, packaging, and installer are implemented and locally tested. |
| Local Windows release candidate | **97%** | The exact ZIP and installer build, validate, install, start, and pass clean-user smoke checks. |
| Private beta readiness | **94%** | Suitable for trusted testers who understand that Microsoft owner sign-in and real Minecraft/game validation still need owner testing. |
| Official public launch readiness | **82%** | The live protected backend is deployed and verified for unauthenticated rejection; signing, authenticated provider verification, and owner-authenticated Minecraft/game testing remain. |
| Whole advertised product vision | **87%** | Production cloud hosting, broad real-machine/game soak coverage, support operations, and public legal/release infrastructure remain beyond this local candidate. |

## Completed in this pass

- Implemented an authenticated Supabase Edge proxy for CurseForge. The
  `CURSEFORGE_API_KEY` stays backend-only; the launcher ships only its public
  Supabase URL/key and uses the signed-in Amalgam user's access token.
- Added strict CurseForge route/query allowlists, rate limiting, timeout and
  response-size limits, sanitized errors, and tests.
- Fixed clean-install configuration. `launcher.json.template` now seeds the
  first private per-user config instead of being ignored.
- Confirmed first GUI launch writes BOM-free JSON, DPAPI-protects the public
  Supabase key locally, and does not create CurseForge, Modrinth, service-role,
  or AI secrets.
- Fixed UTF-8 compilation so the welcome modal and launcher text no longer
  show replacement diamonds or broken punctuation.
- Fixed responsive overflow in Mod Manager, Server Console, and Bedrock action
  rows; fixed the profile Loader health state and the globe icon geometry.
- Verified the launcher at 1440x900 across all 22 routes and at 1024x720 across
  eight high-risk routes. Resizing remains crisp and reflows correctly.
- Built all 19 shipped bridges: 9 Fabric, 4 Forge, and 6 NeoForge.
- Built the branded Inno Setup installer with license, install information,
  privacy/ownership acknowledgement, version metadata, uninstaller, optional
  shortcuts, generated art, manifests, SBOM, and integrity hashes.
- Added final Bedrock archive verification: the outer `.mcaddon` and both
  nested behavior/resource packs must contain their manifests before packaging
  succeeds.
- Shipped the validated Bedrock client, package inventory, and package hash in
  both the release ZIP and the Inno installer.
- Readiness now reports exact 19/19 supported bridge coverage, bundled Bedrock
  client availability, and release-integrity manifest presence.
- Installed the final setup into an isolated folder and confirmed its launcher
  is byte-identical to the staged release binary.
- Uninstalled and reinstalled that copy with a sentinel profile file. Installed
  binaries were removed, while the private config and `instances` user data
  survived both uninstall and reinstall.
- Connected the correct live Supabase project and deployed all 24 Edge Functions
  with JWT verification enabled on every private route; the Whop webhook remains
  the only intentionally public function.
- Verified the live Supabase Auth endpoint and confirmed the live CurseForge
  route rejects requests without a user session. Removed anonymous execute access
  from the legacy security-definer session/project RPCs while preserving
  authenticated and service-role access.

## Final verification matrix

| Gate | Result |
|---|---|
| Native CTest | **35/35 passed** |
| Release/security Node tests | **38/38 passed** |
| Runtime-agent tests | **24/24 passed** |
| Production schema source checks | **65/65 passed** |
| Supabase source inventory | **18 migrations, 24 Edge Functions; passed** |
| Fabric bridges | **9/9 built**: 1.18.2, 1.19.2, 1.20.1, 1.21.1, 1.21.4, 1.21.5, 1.21.6, 1.21.8, 1.21.11 |
| Forge bridges | **4/4 built**: 1.12.2, 1.18.2, 1.19.2, 1.20.1 |
| NeoForge bridges | **6/6 assembled**: 1.21.1, 1.21.4, 1.21.5, 1.21.6 beta, 1.21.8, 1.21.11 |
| Release ZIP validation | **Passed**: files, 19 bridges, Bedrock client package, art manifest, SBOM, hashes, public config, secret exclusions |
| Clean extracted runtime | **Passed**: Java and Windows prerequisites |
| Installer compile | **Passed** with Inno Setup 7.1.0 |
| Installer smoke | **Passed**: exit 0, 19 bridges, art, uninstaller, public config, service key absent |
| Uninstall/reinstall preservation | **Passed**: launcher removed/restored; private config and instance sentinel preserved |
| Installed launcher startup | **Passed**: branded GUI opened with an empty real library and clean beta welcome |
| Installed prerequisites | **Passed**: tar, native DLL, Windows SQLite, MSVC runtime |
| Installed Java detection | **Passed**: system Java 17 detected |
| Installed Bedrock detection | **Passed**: Minecraft for Windows and addon data folder detected |
| Live Modrinth search | **Passed**: 43 result records in the final installed smoke run |
| UI render audit | **22/22 routes passed** at 1440x900; minimum-window sample passed at 1024x720 |
| Secret scan | **8,718 text files scanned; 0 unexpected HIGH findings** |
| Authenticode | **Blocked**: launcher, DLL, and installer are currently unsigned |
| Live CurseForge proxy | **Deployed and protected**: live endpoint returns 401 without a user session; signed-in provider response still needs an owner-authenticated check |
| Microsoft/Minecraft owner launch | **Blocked**: requires an interactive account that owns Minecraft |

The Minecraft asset CDN stalled during one fresh-cache NeoForge `build` asset
task. A subsequent online `assemble`, which exercises the bridge source without
the unrelated asset download, completed all six versions successfully. This is
an upstream asset-delivery observation, not a bridge compile failure.

## Release artifacts

- `dist/amalgam-3.0.0-beta.1.zip`
  - Size: 66,459,536 bytes
  - SHA-256: `212bff2a27cf7f1b4f01795df3d15854adcbac0f7637ebde9aec1be63e950f40`
- `dist/installer/AmalgamLauncher-3.0.0-beta.1-Setup.exe`
  - Size: 62,412,692 bytes
  - Product version: `3.0.0-beta.1`
  - File version: `3.0.0.1`
  - SHA-256: `b0e53fff0f638e22e21fcb796f931bf6daa381044f45c3fbc72531ec5bf4bd77`
- `dist/amalgam-3.0.0-beta.1/bedrock/AmalgamBedrockClient.mcaddon`
  - SHA-256: `88c85ecb8792e10887b168719b8821797e1fb914c3007bdd31cbc194ceca47a1`
- `artifacts/final-route-snapshots-2026-08-22/`
  - 22 full route captures, all larger than 59 KB.
- `artifacts/minimum-window-verification-2026-08-22/`
  - Eight 1024x720 responsive captures.
- `artifacts/installer-smoke/AmalgamLauncher-20260822/`
  - Isolated installation used for the final installed-runtime and GUI checks.

## External gates before public distribution

### 1. Finish live Supabase verification

Target project: `nnrrmvaxnoknthpwvttt`.

The correct Amalgam project is now connected. All 24 Edge Functions are
deployed, and the live Auth endpoint plus unauthenticated CurseForge rejection
checks pass. The backend still needs one authenticated provider test and a
review of remaining Supabase advisor findings before public release.

Required owner action:

1. Sign in to Amalgam with a test account and verify a real CurseForge catalog
   search/download through the protected proxy.
2. Run the live User A/User B RLS, rate-limit, quota, admin-mask, and provider
   tests documented in `docs/live-test-procedures-2026-08-21.md`.
3. Review the remaining security-advisor warnings, especially mutable function
   search paths and intentionally policy-less service-owned tables.

After the authenticated provider check passes, signed-in Amalgam users can
browse both Modrinth and CurseForge.

### 2. Sign every public binary

There is no valid code-signing certificate in the current Windows user store.
Sign `amalgam_launcher.exe` and `amalgam.dll` before generating manifests and
packages, then sign and timestamp the final installer. Re-run package, hash,
installer, and `tools/verify-signing.ps1 -RequireSigned` gates afterward.

### 3. Run owner-authenticated Minecraft end-to-end tests

Authentication cannot be safely automated. A human tester with a Microsoft
account that owns Minecraft must complete device/browser sign-in and verify:

- entitlement/profile exchange and token refresh;
- clean Vanilla Java launch to the main menu;
- representative Fabric, Forge, and NeoForge launches with the matching bridge;
- client DLL/bridge handshake, HUD, menu, diagnostics, and clean shutdown;
- profile update, rollback, logs, screenshots, and crash-report paths.

### 4. Complete live Bedrock and server soak tests

Bedrock detection and addon code tests pass, but the owner should launch the
installed game, import disposable `.mcpack`, `.mcaddon`, and `.mcworld` files,
and restore a backup. Local server creation/start/console/stop/backup should run
for several hours with disposable worlds and supported server types.

### 5. Public operations and legal review

Before a stable public release, verify the website, privacy policy, support and
bug-report destinations, update manifest/CDN, publisher identity, Minecraft and
provider trademark wording, CurseForge/API terms, crash/analytics consent, and
the installer license text with the release owner or counsel.

## Launch recommendation

Use this artifact only as a **closed beta candidate** until Gates 1–3 are
complete. After the backend is deployed, signatures verify, and the real
Minecraft owner matrix passes, repeat the exact package/installer smoke tests
and promote the same code to an RC or stable version.
