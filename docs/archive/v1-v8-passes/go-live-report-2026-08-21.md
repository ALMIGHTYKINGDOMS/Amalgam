# AMALGAM V3 BETA 1 — FINAL GO-LIVE REPORT

Version: 3.0.0-beta.1 · Channel: beta · Date: 2026-08-21

## Gate Results

| Gate | Result |
|---|---|
| SUPABASE LIVE | **FAIL (BLOCKED EXTERNAL)** — no `SUPABASE_ACCESS_TOKEN`, no DB password in the pooler URL, no Supabase CLI installed, no credential files in the workspace. The linked project (`nnrrmvaxnoknthpwvttt`) is intact; `tools/deploy-supabase.sh` is ready to run the moment credentials exist. |
| MICROSOFT AUTH | **FAIL (BLOCKED EXTERNAL)** — requires a real Minecraft-owning Microsoft account + a public Application (client) ID. No account exists in this environment. |
| REAL VANILLA LAUNCH | **FAIL (BLOCKED EXTERNAL)** — requires the above login plus a display session; not possible here. |
| FABRIC OWNER SMOKE | **NOT RUN** (blocked on Gate 2/3) |
| FORGE OWNER SMOKE | **NOT RUN** (blocked on Gate 2/3) |
| NEOFORGE OWNER SMOKE | **NOT RUN** (blocked on Gate 2/3) |
| CI INSTALLER | **FAIL (BLOCKED EXTERNAL)** — requires GitHub Actions (push/trigger). Inno compiler (ISCC) is not installed locally, so the official installer can only be built in CI. |
| INSTALLER SMOKE TEST | **FAIL (BLOCKED EXTERNAL)** — requires the CI-produced installer + a real machine. |

## FINAL RESULT: WAVE 1 BETA — **NO-GO** (not yet)

The three mandatory live gates are all blocked on credentials/accounts
that do not exist in this environment. Nothing is locally broken: the
fresh build (333/333), the full test matrix (160 PASS / 0 FAIL), and the
package all pass. The go-live is gated purely on external resources.

## What closes each gate (exact blockers)

1. **Supabase:** run `bash tools/deploy-supabase.sh` (or `supabase db push` +
   `supabase functions deploy`) with a linked CLI / `SUPABASE_ACCESS_TOKEN`,
   then `node tools/verify-production-schema.mjs` against the live project.
   Live User A/User B security checks are scripted in
   `docs/live-test-procedures-2026-08-21.md` (RLS isolation, no subscription/
   TURN writes, ownership on social RPCs, rate limits, admin email masking,
   feedback RPC, no self-granted entitlements).
2. **Microsoft:** provide `AMALGAM_MICROSOFT_CLIENT_ID` (the packaging step
   injects it into `launcher.json.template`), then run the real device/
   browser login on an account with Java Edition.
3. **Vanilla launch:** after Gate 2, create a clean Vanilla profile with
   managed Java and reach the main menu; record version/java/memory/logs.
4. **CI installer:** push to the repo so the workflow builds the signed
   installer; download the artifact and install it on a real machine.

## Deliverables already in place (locally verified)

- Fresh beta build: `cpp/build-beta` (333/333, 0 errors).
- Verification package: `dist/amalgam-offline-pass/` — launcher, client DLL,
  19 bridge jars, branding, docs, `launcher.json.template`,
  `prerequisites.json`, `BETA-SETUP-NOTES.txt`; 40 files with SHA256SUMS.txt
  + inventory.json; secret scan clean; package JSON valid.
- Docs: `AMALGAM-V3-BETA-1-RELEASE-NOTES.md`, `BETA-TESTER-GUIDE.md`,
  `beta-release-candidate-report-2026-08-21.md`, `live-test-procedures-2026-08-21.md`.
- Known limitations testers will see (documented, gated honestly):
  Essentials relay = beta, Quilt = experimental, Cloud = coming soon,
  Bedrock = requires Minecraft for Windows, paid plans = disabled.

## Action for the owner

Drop one of these into the environment and I will run the corresponding
gate immediately: a Supabase access token or DB password (Gate 1), the
Microsoft client ID + a test account (Gates 2–3), or a push/run of the CI
workflow (Gate 4).

---

## Update — Owner Unlock / Go-Live Execution pass (same day)

### Release naming fixed (locally verified)

The canonical tester installer is now **`AmalgamLauncher-3.0.0-beta.1-Setup.exe`**:

- `installer/AmalgamLauncher.iss` — default `OutputBaseFilename` changed to
  `AmalgamLauncher-3.0.0-beta.1-Setup`; AppVerName remains "Amalgam V3 Beta";
  AppVersion `3.0.0-beta.1`; numeric `VersionInfoVersion` `3.0.0.1`.
- `.github/workflows/build.yml` — the signing-verification steps and artifact
  upload now glob `AmalgamLauncher-*-Setup.exe` instead of hardcoding the
  V2 name, so the beta filename flows through automatically.
- `docs/release.md` + tester guides updated; zero remaining `AmalgamLauncher-V2`
  references anywhere (verified by grep).
- About page shows "Version 3.0.0-beta.1 (BETA)"; release notes and update
  manifest tooling already keyed to 3.0.0-beta.1.
- Tests re-run after the packaging-source changes: CTest 33/33, runtime-agent
  24/24, schema 65/65, tooling 38/38 — 160 PASS / 0 FAIL intact.
- Package re-hashed (40 files) and secret-scan clean after doc refresh.

### Gate status (re-checked this pass)

Credentials/accounts/CI were probed again at the start of this pass — no
Supabase token, no DB password, no Microsoft client ID/account, no CI push.
All mandatory gates remain externally blocked.

| Gate | Result |
|---|---|
| SUPABASE DEPLOY | FAIL (BLOCKED — no access token / DB password; deploy script ready) |
| LIVE SECURITY VERIFICATION | FAIL (BLOCKED — requires deployed project) |
| CI V3 BETA INSTALLER | FAIL (BLOCKED — needs GitHub Actions run; naming now correct) |
| INSTALLER SMOKE | FAIL (BLOCKED — needs CI installer + a machine) |
| MICROSOFT AUTH | FAIL (BLOCKED — needs account + client ID) |
| REAL VANILLA LAUNCH | FAIL (BLOCKED — needs Gates above) |
| OPTIONAL FABRIC / FORGE / NEOFORGE | NOT RUN |

### WAVE 1 BETA: NO-GO

Nothing is locally blocking: fresh build 333/333, 160 PASS / 0 FAIL, package
validated and clean. The single remaining dependency is owner-supplied
credentials (Supabase token/password) and accounts (Microsoft client ID +
Java-owning account), plus one CI run to produce the official installer.
The moment any of those appear, the corresponding gate can execute.
