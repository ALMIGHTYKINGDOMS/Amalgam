# AMALGAM V3 — BETA RELEASE CANDIDATE REPORT

VERSION: 3.0.0-beta.1
BUILD: fresh `cpp/build-beta` (333/333) — Debug, Ninja, MSVC 19.51, `AMALGAM_STRICT_HARDENING=ON`
CHANNEL: beta

## Build

| Item | Result |
|---|---|
| Fresh build | PASS — brand-new `cpp/build-beta` directory, full configure + 333/333 steps, 0 errors |
| Steps | 333/333 |
| Errors | 0 |
| Warnings | 55 /W4 — all classified (see prior report); 0 from new beta code |

## Tests (fresh beta build)

| Suite | Result |
|---|---|
| CTest | 33/33 PASS |
| Runtime Agent | 24/24 PASS |
| Schema verifier | 65/65 PASS |
| Supabase source | PASS (18 migrations, 23 Edge Functions) |
| Node tooling (manifest / rate-limit / TURN) | 38/38 PASS |
| Updater + atomic-config (C++) | PASS |

TOTAL: 160 PASS / 0 FAIL / 0 SKIP.

## Beta Core

| System | Status |
|---|---|
| Home | READY — real data with loading/empty/error/offline states; news feed labeled as fallback |
| Discover | READY — live Modrinth + CurseForge search, details, dependencies, install |
| Library | READY — profile lifecycle, content management, updates |
| Downloads | READY — persistent jobs, retry, resume, failure recovery |
| Profiles | READY — create/import/edit/play, atomic config, corrupted-config recovery |
| Java | READY — Runtime Manager (8/11/17/21/25), managed downloads, DPAPI |
| Bedrock | BETA — fully gated when Minecraft for Windows is absent; no broken controls |
| Essentials | BETA — friends/sessions real; direct P2P exposed; relay state shown honestly |
| Local Servers | READY — Paper/Purpur/Folia/Fabric/Quilt auto-provisioned; Vanilla/Spigot labeled manual |
| Client | READY — Insert-key menu, HUD + editor, persisted settings, no fake systems |
| Updater | READY (code) — signed manifest, staged + verified, anti-rollback, rollback, beta channel guard; live manifest/signing pending release engineering |
| Diagnostics | READY — local logs, `--doctor`, feedback with local-save fallback |

## Beta Safety

| Item | Result |
|---|---|
| Config recovery | Atomic writes + `.bak` restore, stress-tested (1000 cycles) |
| Updater verification | RSA-SHA256, size+hash, tamper/wrong-key/signed-no-key fail-closed, path-traversal guard |
| Secret scan | Clean — 0 unexpected findings in repo or package |
| Crash recovery | New: `.amalgam_running` marker → "did not close normally" dialog (Continue / Open Logs / Safe Mode) |
| Safe mode | New: `--safe-mode` skips auto update check + catalog prefetch; SAFE MODE indicator; no data erased |
| Logs | Timestamped, rotated (JSONL, 5 MB × 4), no tokens/keys logged |
| Offline | Local features usable; network pages show offline + retry; no request storms |

## New in this release (closed-beta build pass)

- Canonical identity: `kVersion = "3.0.0-beta.1"`, `kChannel = "beta"`; update policy uses the real channel (beta builds get beta updates; stable can never jump to beta).
- First-run **Welcome to Amalgam Beta** dialog (Continue / Known Issues / Report a Bug), dismissed via `launcher.json`.
- **Beta badge** in the sidebar footer and Settings → About ("Version 3.0.0-beta.1 (BETA)").
- **Known Issues** dialog — packaged, real limitations (Essentials relay beta, Quilt experimental, Cloud coming soon, Bedrock gating, signed channel, beta accounts); reachable from About and the welcome dialog.
- **Report a Bug** entry points in About + welcome; existing feedback dialog submits via `submit_beta_feedback` with local-save fallback.
- **Crash recovery + safe mode** (above).
- Installer branded **"Amalgam V3 Beta"** with numeric `3.0.0.x` version resources; CI stamps `3.0.0.1` by default (overridable via `AMALGAM_INSTALLER_VERSION`).

## Gated From Beta

- **Cloud** — Coming Soon; launcher is a read-only landing; no purchase/deploy/fake provisioning.
- **Whop / paid purchases** — disabled.
- **TURN relay** — labeled beta; issuance gated server-side; not falsely advertised.
- **Quilt** — experimental label.

## Remaining Live Checks Before Tester Invite

1. Supabase deployment (CLI + access token) and live schema verification.
2. Real Microsoft/Minecraft login.
3. At least one real Java launch (Vanilla + Fabric).
4. Optional for wave 0/1: Bedrock detection on a machine that has it; Essentials direct P2P between two networks; signed update delivery.

## Known Issues

See `AMALGAM-V3-BETA-1-RELEASE-NOTES.md` (also shipped in the package) — Essentials relay beta, Quilt experimental, Cloud coming soon, Bedrock requires MCfW, beta accounts.

## Tester Package

| Item | Status |
|---|---|
| Installer | Inno script branded "Amalgam V3 Beta"; compiled by CI (ISCC not locally installed) |
| Archive | `dist/amalgam-offline-pass/` — launcher, DLL, 19 bridges, branding, docs; SHA256SUMS.txt + inventory.json (37 files) |
| Release Notes | `AMALGAM-V3-BETA-1-RELEASE-NOTES.md` |
| Tester Guide | `BETA-TESTER-GUIDE.md` |
| Hashes | SHA-256 for every file; secret scan clean |

## Final Decision

**BETA TESTER RELEASE: READY** — for Wave 0/1 (developer + 5–10 trusted testers), subject to the three live checks above (Supabase deploy, real MS login, one real Java launch). All local gates are satisfied: fresh build, all tests, installer/package produced, no user-visible placeholders, no data-loss/auth P0 known, atomic config + updater verified, Cloud/paid gated, diagnostics + bug reporting available, version consistently marked beta, package secret scan passes.
