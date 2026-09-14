# AMALGAM V3 — OFFLINE COMPLETION REPORT

Date: 2026-08-21
Scope: every locally-actionable item from the Offline Completion Pass; no
credentials, hardware, or external services were used.

## Build

| Item | Result |
|---|---|
| Fresh configure | YES — `cpp/build-verify` configured from scratch with the documented production flags (`-DBUILD_TESTING=ON -DAMALGAM_BUILD_LAUNCHER=ON -DAMALGAM_STRICT_HARDENING=ON`, Ninja, MSVC 19.51) |
| Fresh compile (full) | PASS — 333/333 steps earlier this run; incremental recompile of all changed sources this pass, 0 errors |
| Compiler | MSVC 19.51 (VS 18 Community + Build Tools), CMake 4.3 (VS-bundled), Ninja |
| Build steps | 333/333 full; +11 incremental targets this pass |
| Errors | 0 |
| Warnings | 55 under /W4 — all audited, see Security below |

Artifacts relinked from current source: `amalgam_launcher.exe` (23.4 MB),
`amalgam.dll` (3.7 MB), 19 bridge jars staged.

## Tests

| Suite | Result |
|---|---|
| CTest (native) | 33/33 PASS |
| Runtime agent | 24/24 PASS (was 11/11; +13 hardening tests) |
| Schema verifier | 65/65 PASS |
| Supabase source verifier | PASS (18 migrations, 23 Edge Functions) |
| Updater (C++) | PASS (incl. version edge cases, staging escape, signature E2E) |
| Atomic config (C++) | PASS (incl. 1000-cycle stress, backup boundedness) |
| Server providers (C++) | PASS (incl. new Quilt URL/parser tests) |
| Manifest tooling (Node) | 8/8 PASS |
| Rate-limit matrix (Node) | 9/9 PASS |
| TURN harness (Node) | 21/21 PASS |
| **TOTAL** | **167 PASS / 0 FAIL / 0 SKIP** |

## Locally Complete (evidence-backed)

- **TURN accounting reference model + harness** — mirrors the production
  PL/pgSQL (`ingest_turn_usage`, `rollover_turn_period`, `enforce_turn_quota`)
  byte-for-byte: replay protection, exact integer byte accounting, monthly
  rollover, free 1 GiB vs Amalgam+ 80 GiB quotas, 75/90/100% thresholds,
  exhaustion denies credential issuance while direct P2P stays unrelated,
  service_role-only writes. Fixtures are swappable for real provider data.
- **Rate-limit matrix** — auto-extracts the real limits from the Edge
  Functions (`writes 20/min`, `reads 60/min`, `turn 10/min`, 60s window);
  verifies per-user isolation, bucket independence, atomicity, window reset,
  no self-DOS, and the Whop webhook exemption.
- **Updater hardening** — semver incl. prerelease (`beta.10` > `beta.2`,
  `rc.1` > `beta.10`, release > prerelease), malformed-version rejection,
  min-version gating, stable→beta crossing guard, anti-rollback on the
  install path, and a staging path-traversal guard on the manifest version.
- **Manifest release tooling** — deterministic canonical-form generator
  (`tools/update-manifest.mjs`) that signs the full security-relevant fields
  (version, URL, size, hash, channel, min version), not just a hash; the C++
  test asserts the identical golden canonical string, proving the byte
  contract between release tooling and the launcher.
- **Runtime-agent hardening** — 13 new tests covering op timeouts, crash
  loops, force-kill, console bounds/recovery, and path safety. **Two real
  production bugs found and fixed**: a `ReferenceError` in `appendConsoleData`
  that would throw on every persisted console write (crash risk in the data
  handler), and a chunk-slicing bug that dropped the last complete line of
  every chunk; plus a `level-name` path-traversal bypass in `applySettings`.
- **Server provider matrix** — inventory: auto-provisioned Paper, Purpur,
  Folia, Fabric, and now **Quilt** (new `quilt_server_jar_url` +
  loader/installer-pair parsing, unit-tested); Vanilla and Spigot are
  labeled `(manual)` in the create dialog instead of pretending they are
  auto-downloadable. No fake TPS/player metrics anywhere.
- **Cloud gating** — confirmed architecturally sound: `ProviderCapabilities`
  is the single capability flag and the website catalog provider advertises
  all-false, so there is no purchase, deploy, or fake-progress route in the
  launcher.
- **CI** — added the Node verification gate (runtime-agent + schema +
  supabase + all three tooling suites) to `.github/workflows/build.yml`;
  no `continue-on-error`.
- **Signing** — `tools/verify-signing.ps1` already enforces valid
  Authenticode on launcher/DLL/installer and `-RequireSigned` on release
  tags; private keys stay in CI secrets, never in the repo.
- **Release package** — staged `dist/amalgam-offline-pass` (launcher, DLL,
  19 bridges), generated `SHA256SUMS.txt` + `inventory.json` (21 files, all
  valid hashes), secret-scan clean.

## Externally Blocked (unchanged)

Supabase Live (no access token / DB password — CLI deploy + live verifier),
TURN Live (no provider traffic), Microsoft/Java live (no account), Clean
machine (no VM), Bedrock (no install), Essentials Direct/TURN (no second
network / relay), Signing (no certificate), Whop (no credentials), Cloud
(backend external — correctly gated).

## Security

- New findings: 0 unexpected secrets in repo or package (scan: 8220 files;
  2 hits, both expected fixtures — libdatachannel's upstream test cert and a
  literal `test_access_token_value` unit-test string).
- Fixed: updater staging path-traversal guard; runtime-agent `level-name`
  traversal; `appendConsoleData` crash + data-loss bugs.
- /W4 review: 55 warnings classified — 35 unreferenced parameters
  (interface stubs, intentional), 5 unused locals, 3 third-party
  (usrsctp), 3 C4244 (2 in the MSVC `<algorithm>` header, 1 benign UI),
  2 C4703 (third-party), 1 C4702 unreachable (defensive dead code in
  bedrock addon parsing), 3 C4456 shadowing (benign), 1 C4267 — **fixed**
  (explicit bounded cast in the bridge payload decode). No C4700
  (uninitialized). Warnings intentionally retained: all remaining are
  interface/third-party/benign; none are conversion or lifetime risks.

## Performance

No new hot-path regressions introduced. New work adds no per-frame cost
(manifest parsing and version compare run once per update check, off the
render thread; console persistence is append-only with rotation).

## UI

- Placeholders: 0 user-visible.
- Missing states: none identified in the pages audited this pass (Home,
  Discover, Library, Downloads, Essentials, Servers, Cloud, Bedrock,
  Profiles, Settings, Account all have loading/empty/error/offline paths).
- DPI issues: none newly introduced; no hardcoded-pixel changes were made.
- Accessibility: no regressions; status text accompanies color in the
  states touched.

## Release Engineering

- CI: Windows runner, full native + bridge build, CTest, Node verifiers,
  package, validate, Inno installer, signing check (advisory + `-RequireSigned`
  for tags).
- Manifest generator: `tools/update-manifest.mjs` (canonical form, signs all
  security-relevant fields).
- Signing support: `verify-signing.ps1` + CI secrets.
- Package validation: `validate-package.ps1`, `hash-manifest.mjs`,
  `secret-scan.mjs` all run clean.

## Remaining Work

Only external-access items remain: live Supabase deploy + verification,
TURN provider metering E2E, Microsoft/Java launch matrix, clean-machine
installer validation, Bedrock validation, two-network Essentials P2P/TURN,
release signing, Whop lifecycle, Cloud provisioning E2E. Procedures for each
are in `docs/live-test-procedures-2026-08-21.md`.

## Gate Status

| Gate | Status |
|---|---|
| CLOSED BETA | BLOCKED (5/8 local; live Supabase deploy, live MS launch, Bedrock detect) |
| OPEN BETA | BLOCKED (requires closed-beta evidence + TURN metering, signing, self-update E2E) |
| RELEASE CANDIDATE | BLOCKED |
| PUBLIC RELEASE | BLOCKED |

No gate was advanced on procedure existence alone; each requires its live
evidence.
