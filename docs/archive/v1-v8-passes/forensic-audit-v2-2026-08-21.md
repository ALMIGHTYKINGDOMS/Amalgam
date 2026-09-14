# AMALGAM V3 — FORENSIC AUDIT V2

Date: 2026-08-21 (revision 2)
Auditor: deepseek-v4-pro (max effort), assisted by the same session's earlier
V1-era audit work and the launcher optimization pass.
Method: source-truth audit. Every conclusion is tied to a file/symbol and an
evidence level. Where verification was impossible in this environment it is
marked **BLOCKED — ENVIRONMENT**, never guessed.

---

## 1. Audit Metadata

| Item | Value |
|---|---|
| Environment | Freebuff desktop agent shell (bash on Windows), Node v24 available; NO MSVC/Ninja/CMake, NO Gradle/JDK, NO Supabase CLI, NO Deno, NO Minecraft, NO Bedrock UWP, NO Microsoft account |
| Fresh build attempted | No — toolchain absent |
| Fresh build status | **BLOCKED — ENVIRONMENT** (CMake/MSVC/Ninja not installed in shell) |
| Build artifacts inspected | `cpp/build/` (launcher + DLL + 19 bridge jars, dated 2026-08-21) — exists but NOT proof current source builds |
| Tests discovered | 31 CTest checks (26 native + 2 launcher CLI smoke + 3 Java bridge protocol), 7 runtime-agent Node tests, 1 Java `ProtocolTest` |
| Tests executed here | `verify-supabase.mjs` (PASS: 17 migrations, 23 Edge Functions); runtime-agent `npm test` (PASS 7/7); brace-balance + symbol checks on edited files |
| Tests NOT executed | All CTest native suites (need MSVC build) — **BLOCKED**; Gradle bridge builds — **BLOCKED** |
| External systems | Supabase (live): unavailable; Whop: unavailable; Cloudflare TURN: unavailable; amalgam-mc.com website: unverified; Modrinth/CurseForge: not exercised live |
| Audit limitations | No live E2E of any kind; no clean machine; no real account; no signed artifacts |

---

## 2. Executive Verdict

| Metric | V2 |
|---|---|
| Overall Completion | 68% |
| Closed Beta Readiness | 40% |
| Open Beta Readiness | 25% |
| Public Release Readiness | 18% |
| AAA Quality | 48/100 |
| Prototype Index | 30/100 (higher = more prototype-like) |
| Security Risk | 44/100 (higher = riskier) |
| Release Risk | 68/100 |

**FINAL STAGE: INTERNAL ALPHA.**

Not because the launcher is a toy — the launcher's happy path is genuinely
substantial — but because the strict release gates are binary and none of the
live ones are satisfied: no live Microsoft account launch, no clean-machine
run, no code signing, no live Bedrock, no deployed backend, no authoritative
TURN accounting, no self-updater. Per the audit's own rules those are release
gates, not polish items.

---

## 3. Audit V1 vs Audit V2

| METRIC | V1 | V2 | CHANGE | REASON |
|---|---|---|---|---|
| Overall | 72% | 68% | −4 | V1 counted static presence; V2 applied evidence caps and found unwired systems (rate limiting, TURN accounting) and placeholders |
| Beta readiness | 55% | 40% | −15 | V1 treated "code exists + dry-runs pass" as beta; V2 requires live MS account, clean machine, backend deploy — all absent |
| Public release | 25% | 18% | −7 | Same gates plus updater, signing, TURN/Cloud products unimplemented |
| AAA quality | 55% | 48% | −7 | Hardcoded Home news, in-client placeholders (Survival Squad, Map Coming Soon), toast-only controls |
| Prototype index | 38 | 30 | −8 | Launcher is more real than V1 feared (schema complete, bridges real); client overlay still has ~10 placeholder items |
| Technical debt | 55 | 58 | +3 | ui.cpp 12.7k lines, stale docs, non-atomic config writes, duplicated social systems |
| Security risk | 40 | 44 | +4 | Rate-limit helper unwired, no TURN accounting, staff directory exposes emails, no supply-chain signing |

---

## 4. Master Scoreboard (capped)

Formula per system: impl 20 / function 20 / integration 15 / reliability 10 /
test 10 / security 10 / UI 5 / performance 5 / maintainability 5, then the
evidence cap. Raw vs capped both shown.

| SYSTEM | IMPL | FUNC | INT | REL | SEC | TESTS | UI | PERF | MAINT | RAW | CAP | FINAL | EVID | CONF |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Launcher Shell | 92 | 88 | 80 | 78 | 80 | 70 | 92 | 80 | 55 | 83 | 75 | 75 | E2/E3 | HIGH |
| Home | 85 | 78 | 60 | 70 | 80 | 40 | 85 | 80 | 60 | 72 | 60 | 60 | E2 | MED |
| Discover | 88 | 82 | 70 | 65 | 78 | 55 | 84 | 70 | 60 | 76 | 75 | 75 | E2/E3 | MED |
| Library / Profiles | 86 | 82 | 72 | 75 | 80 | 55 | 82 | 78 | 60 | 77 | 75 | 75 | E2/E3 | MED |
| Profile Wizard | 88 | 80 | 65 | 70 | 78 | 45 | 88 | 75 | 60 | 75 | 60 | 60 | E2 | MED |
| Downloads | 85 | 78 | 70 | 80 | 80 | 70 | 80 | 65 | 60 | 77 | 75 | 75 | E2/E3 | MED |
| Java Runtime | 90 | 84 | 75 | 80 | 85 | 75 | 80 | 70 | 70 | 81 | 75 | 75 | E3 | HIGH |
| Vanilla launch | 82 | 70 | 60 | 70 | 78 | 40 | 75 | 70 | 60 | 70 | 60 | 60 | E2 | LOW |
| Fabric | 86 | 72 | 60 | 65 | 80 | 55 | 75 | 70 | 60 | 71 | 60 | 60 | E2/E3 | MED |
| Quilt | 55 | 30 | 20 | 30 | 70 | 0 | 50 | 40 | 50 | 39 | 20 | 20 | E1/E2 | LOW |
| Forge 1.12.2–1.20.1 | 85 | 70 | 55 | 65 | 80 | 55 | 72 | 70 | 60 | 70 | 60 | 60 | E2/E3 | MED |
| NeoForge | 86 | 72 | 60 | 65 | 80 | 55 | 75 | 70 | 60 | 72 | 60 | 60 | E2/E3 | MED |
| Mods (Modrinth/CF) | 90 | 82 | 75 | 72 | 78 | 60 | 82 | 70 | 60 | 78 | 75 | 75 | E2/E3 | MED |
| Modpacks | 82 | 72 | 65 | 70 | 75 | 50 | 75 | 60 | 55 | 71 | 75 | 71 | E2 | MED |
| Shaders/Res/Data packs | 78 | 68 | 55 | 60 | 72 | 40 | 70 | 60 | 55 | 64 | 60 | 60 | E2 | LOW |
| Worlds (launcher) | 80 | 70 | 55 | 65 | 75 | 35 | 78 | 65 | 55 | 66 | 60 | 60 | E2 | MED |
| Screenshots (launcher) | 80 | 70 | 55 | 65 | 75 | 35 | 78 | 65 | 55 | 66 | 60 | 60 | E2 | MED |
| Logs | 78 | 68 | 50 | 65 | 72 | 30 | 70 | 70 | 60 | 63 | 60 | 60 | E2 | LOW |
| Bedrock | 82 | 40 | 20 | 40 | 70 | 40 | 70 | 50 | 55 | 52 | 40 | 40 | E1/E2 | LOW |
| Essentials UI (launcher) | 88 | 74 | 70 | 65 | 78 | 50 | 90 | 70 | 55 | 75 | 60 | 60 | E2 | MED |
| Friends / Requests | 85 | 60 | 60 | 55 | 80 | 30 | 82 | 60 | 55 | 66 | 60 | 60 | E2 | MED |
| Invites / Sessions | 85 | 60 | 60 | 55 | 75 | 30 | 78 | 60 | 55 | 64 | 60 | 60 | E2 | LOW |
| Parties | 82 | 55 | 55 | 50 | 75 | 25 | 75 | 55 | 50 | 61 | 60 | 60 | E2 | LOW |
| Messaging (launcher) | 82 | 55 | 55 | 50 | 75 | 25 | 78 | 55 | 50 | 61 | 60 | 60 | E2 | LOW |
| Essentials Backend | 88 | 72 | 70 | 65 | 82 | 40 | — | 60 | 60 | 73 | 75 | 73 | E2/E3 | MED |
| P2P (WebRTC bridge) | 78 | 45 | 35 | 40 | 70 | 20 | — | 45 | 50 | 51 | 65 | 51 | E2 | LOW |
| STUN | — | — | — | — | — | — | — | — | — | — | — | n/a (browser/lib default) | — | LOW |
| TURN (credentials) | 80 | 55 | 40 | 40 | 70 | 0 | — | 40 | 55 | 52 | — | — | E2 | MED |
| TURN (accounting) | 5 | 0 | 0 | 0 | 0 | 0 | — | 0 | 30 | 4 | 50 | 4 | E1 | HIGH |
| Local Servers | 82 | 60 | 55 | 55 | 72 | 45 | 78 | 60 | 55 | 65 | 60 | 60 | E2/E3 | MED |
| Cloud UI | 70 | 35 | 25 | 30 | 60 | 35 | 72 | 50 | 50 | 47 | 60 | 47 | E2 | MED |
| Cloud API/Provisioning | 30 | 5 | 0 | 0 | 40 | 0 | — | 0 | 30 | 15 | 20 | 15 | E1/E2 | LOW |
| Billing / Amalgam+ | 55 | 15 | 10 | 10 | 70 | 5 | — | 5 | 40 | 28 | 20 | 20 | E2 | LOW |
| Accounts / Auth | 85 | 70 | 65 | 70 | 82 | 55 | 78 | 65 | 60 | 73 | 75 | 73 | E2/E3 | MED |
| Supabase | 90 | 75 | 70 | 65 | 85 | 60 | — | 55 | 65 | 75 | 75 | 75 | E2/E3 | MED |
| Updater | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10 | 1 | 85 | 1 | E1 (absent) | HIGH |
| Notifications | 85 | 75 | 60 | 70 | 75 | 30 | 80 | 75 | 60 | 69 | 60 | 60 | E2 | MED |
| Diagnostics | 82 | 68 | 55 | 60 | 75 | 50 | 70 | 60 | 55 | 66 | 75 | 66 | E2/E3 | MED |
| Installer | 72 | 50 | 30 | 45 | 60 | 40 | 65 | 50 | 55 | 53 | 80 | 53 | E1/E2 | LOW |
| In-Game Client core | 78 | 60 | 50 | 55 | 72 | 45 | 70 | 70 | 55 | 63 | 60 | 60 | E2/E3 | MED |
| HUD | 80 | 62 | 50 | 55 | 72 | 40 | 78 | 75 | 55 | 64 | 60 | 60 | E2 | MED |
| HUD Editor | 78 | 58 | 45 | 50 | 70 | 30 | 76 | 70 | 55 | 60 | 60 | 60 | E2 | MED |
| Modules (6 native) | 88 | 75 | 65 | 65 | 75 | 55 | 80 | 70 | 60 | 73 | 75 | 73 | E2/E3 | MED |
| Client Social | 45 | 20 | 15 | 20 | 50 | 0 | 40 | 40 | 45 | 30 | 60 | 30 | E1 | HIGH |
| Cosmetics | 55 | 30 | 30 | 30 | 55 | 0 | 50 | 40 | 45 | 37 | 60 | 37 | E1/E2 | MED |
| Client Performance | 78 | 60 | 45 | 50 | 70 | 30 | 72 | 75 | 55 | 61 | 60 | 60 | E2 | LOW |
| Client Settings | 82 | 70 | 55 | 60 | 75 | 30 | 80 | 70 | 55 | 66 | 60 | 60 | E2 | MED |
| Security | 78 | 70 | 65 | 65 | 75 | 50 | — | 55 | 60 | 69 | 80 | 69 | E2/E3 | MED |
| Performance (app) | 82 | 75 | 60 | 70 | 75 | 40 | — | 80 | 60 | 70 | 75 | 70 | E2 | MED |
| Recovery | 85 | 75 | 70 | 80 | 78 | 60 | — | 60 | 60 | 75 | 75 | 75 | E2/E3 | MED |

**Requirement note:** "STUN" is delegated to libdatachannel defaults — no
Amalgam-owned STUN server exists. Not scored separately.

---

## 5. Evidence Ledger (representative)

| System | Evidence | Level |
|---|---|---|
| Wire protocol C++↔Java | `cpp/src/core/protocol.h` static_asserts; `java/common/.../ProtocolTest.java`; `run_java_bridge_test.cmake` compiles real bridge sources with real javac | E3 |
| DPAPI secrets | `auth.cpp` CryptProtectData/UnprotectData; `config.cpp` write_secret; `auth_test.cpp` | E3 |
| Java runtime mgmt | `java.cpp` Adoptium download, SHA-256, extract, `java -version` validation, `runtime.json`; `java_test.cpp` | E3 |
| Supabase schema | 17 migrations; `verify-supabase.mjs` PASS | E3 (source) |
| Runtime agent | `tools/runtime-agent` npm test 7/7 | E3 |
| TURN credentials | `get-turn-credentials/index.ts` real CF TURN API, TTL 60–3600 | E2 |
| TURN accounting | `turn_usage` table: created `...003`, SELECT-only policy, **zero writers anywhere** | E1 |
| Rate limiting | `enforce_rate_limit` defined in `006`/`007`, granted, **never called by any RPC** | E1 |
| WebRTC bridge | `essentials_tcp_bridge.cpp` libdatachannel + Supabase signal poll | E2 |
| Whop webhook | `whop-webhook/index.ts` HMAC `id.timestamp.body`, 300 s replay window, idempotency table | E2 |
| Launcher UI | 12.7k-line `ui.cpp` + per-page TUs; fixture captures documented (not re-run) | E2 |
| Client modules | `core/modules.cpp` 6 modules, all with tick fns; Java controllers (Freecam/Attack/Slot/Velocity) | E2/E3 |
| Client placeholders | `client_ui.cpp` "Survival Squad"/"Alex"/"Sarah"; "Map Coming Soon"; "Messaging coming soon" | E1 |
| Home news | `ui.cpp` hardcoded `NewsItem news[]` | E1 |
| Config persistence | `config.cpp` → `json_write_file` plain trunc-write (non-atomic) | E2 |

---

## 6. V1 Claims Revalidated

| V1 CLAIM | V2 RESULT | EVIDENCE |
|---|---|---|
| Java Runtime Manager substantially complete | CONFIRMED | E3: `java.cpp` full lifecycle + `java_test` |
| DPAPI secret storage properly implemented | CONFIRMED | E3: `auth.cpp` + `write_secret` + `auth_test` |
| CI/release pipeline mature | PARTIALLY CONFIRMED | E1: `.github/workflows/build.yml` exists; **never run in this env** |
| Bedrock support substantial | PARTIALLY CONFIRMED | E1/E2: detect/launch/addon code real; zero live validation |
| Supabase security hardened | PARTIALLY CONFIRMED | RLS + hashed node secrets real (007); **rate limiting unwired** |
| Whop webhook validates HMAC | CONFIRMED | E2: HMAC + replay window + idempotency |
| TURN credentials bounded and authenticated | CONFIRMED | E2: Supabase session + TTL 60–3600 |
| Entitlements backend-authoritative | PARTIALLY CONFIRMED | Cannot self-grant via Supabase (RLS read-only); external API absent by default; subscriptions empty until backend live |
| 24 test executables pass | STALE | 26 native + 2 smoke + 3 Java = 31 registered; **none executed here** |
| TURN usage has no authoritative writer | CONFIRMED | E1: nothing inserts/updates `turn_usage` |
| Launcher self-updater does not exist | CONFIRMED | E1: only mod "Check for Updates" + hardcoded news |
| Cloud backend external/absent | CONFIRMED | `api_url` default empty; dev-only provider |
| Client cosmetics hardcoded | REFUTED | Cosmetics load from `shared_cosmetics.txt` launcher bridge; no demo array |
| Client social/party data hardcoded | PARTIALLY CONFIRMED | Party hardcoded ("Survival Squad"); friends list from launcher bridge |
| Messaging contains Coming Soon | PARTIALLY CONFIRMED | Launcher has real messages tab→edge functions; **client overlay says "Messaging coming soon"** |
| World map contains Coming Soon | CONFIRMED | `client_ui.cpp` "Map Coming Soon" |
| Some displayed client modules have no implementation | REFUTED | All 6 registered modules have tick fns + Java controllers |
| Quilt support uncertain/beta | CONFIRMED | Pinned `quilt-loader 0.20.0-beta.9`; no clean validation |
| Worlds/Screenshots/Logs thin | PARTIALLY CONFIRMED | Launcher galleries real; client pages basic |
| Essentials no NAT/TURN E2E validation | CONFIRMED | No live test possible here |
| Config atomic writes | **V1 omitted — V2 finds NON-ATOMIC** | `json_write_file` trunc+write; crash can corrupt `launcher.json` |
| Rate limiting protects RPCs | **V1 overstated** | Helper exists; zero RPCs call it |

---

## 7–13. System Tiering

**Confirmed production systems (launcher-local, E2/E3):** wire protocol, DPAPI
secrets, Java runtime provisioning, native download/extract safety, profile
scan/recovery, exclusive job queue, 19 bridge jars, server runtime
provisioning (Paper/Purpur/Folia/Fabric), diagnostics outbox, replay,
telemetry, admin auth (PBKDF2), Modrinth/CurseForge installs (staged).

**Nearly complete:** Discover, Library, Downloads, Java, Mods, Recovery flows,
Supabase schema/functions source.

**Partial:** Bedrock (no live), Quilt (beta pin), Essentials networking (no
NAT), Cloud UI (dev provider), Amalgam+ (no live backend), In-game client
social, Cosmetics.

**Prototype:** Cloud backend, billing/E2E entitlement, updater.

**Placeholder/Mock:** client party ("Survival Squad"), client world map
("Map Coming Soon"), client messaging ("coming soon"), Home news (hardcoded),
friend "Invite to World"/"View Profile" (toast-only), `turn_usage` (table only).

**Broken:** nothing is provably broken at the code level; the broken category
is dominated by *unwired* systems (rate limiting, TURN accounting) and
*absent* systems (updater, cloud backend).

**Unknown/Unverified:** live Bedrock behavior; live WebRTC/NAT; live Whop;
live TURN; live MS launch; clean machine; production Supabase deploy;
amalgam-mc.com website/API.

---

## 14. P0 Blockers

| ID | SYSTEM | FINDING | EVIDENCE |
|---|---|---|---|
| P0-1 | TURN accounting | `turn_usage` has zero writers; quota is client-display only; Amalgam+ "80 GB TURN/month" cannot be enforced or even measured | migrations `...003` SELECT-only; no INSERT anywhere |
| P0-2 | Updater | No self-update path; a shipped launcher can only be replaced by manual reinstall | grep: only mod "Check for Updates" |
| P0-3 | Cloud | Cloud backend external/absent; `api_url` default empty; launcher cloud is a dev provider | `online_config.h`; `cloud_provider.cpp` |

## 15. P1 Blockers

| ID | SYSTEM | FINDING | EVIDENCE |
|---|---|---|---|
| P1-1 | Build | Fresh build **BLOCKED** in this env; artifacts predate latest edits | no MSVC/Gradle |
| P1-2 | Java launch | No live Microsoft account launch performed | external gate |
| P1-3 | Bedrock | No UWP package/live validation | external gate |
| P1-4 | Clean machine | No clean-room install/launch evidence | external gate |
| P1-5 | Signing | Launcher/DLL/installer unsigned | external gate |
| P1-6 | Rate limiting | `enforce_rate_limit` never called by any RPC | 006/007 definitions only |
| P1-7 | Client social | Party/map/messaging placeholders visible to users | `client_ui.cpp` |
| P1-8 | Home news | Hardcoded news feed presented as real | `ui.cpp` `NewsItem news[]` |
| P1-9 | Config integrity | `config::save` non-atomic; crash mid-write truncates `launcher.json` | `json.cpp` |
| P1-10 | Entitlements | External API absent by default; subscriptions empty until backend/Whop live | `online_config.h` |
| P1-11 | Staff directory | `admin_list_users` returns emails | migration `...005` |
| P1-12 | Test confidence | Zero E2E/fault-injection/live tests; CTest not executed here | inventory |

## 16. P2 Findings (selected)

P2-1 `ui.cpp` 12.7k lines (documented split). P2-2 Stale audit docs (marked
superseded this session). P2-3 Repo hygiene: two 770 MB `*.hprof` dumps,
`NUL.obj` at root. P2-4 Quilt pinned to beta loader, unproven. P2-5 Split-brain
social (launcher `social_ui` vs client overlay share no state). P2-6 Client
cosmetics ownership derived client-side (display only). P2-7 Performance
optimizations (this session) unmeasured. P2-8 Diagnostics outbox never
uploads (documented contract only).

## 17. P3 Polish

News from backend; empty/error states consistent; accessibility (focus/
keyboard nav); DPI pass at 200 %; icon cache retry-after-failure.

---

## 18–33. Deep Sections (condensed)

**Desktop launcher:** all major pages render with real data paths; controls
are mostly wired (traced Home/Discover/Library/Downloads/Settings/Java/
Servers/Essentials). Notable exceptions: Home news (hardcoded), friend action
buttons in client (toast-only). **AAA Desktop:** strong structure/typography/
state-communication; hardcoded content and no reference comparison cap score
at 52. **Java/Minecraft:** launch path real (`CreateProcessW`, classpath,
natives, assets, dry-runs documented); separated per loader, all E2/E3, no
live run → all loader readiness ≤60. **Content/Downloads:** state machine +
retry/resume/hash/stage/rollback real; E2/E3. **Bedrock:** real detection
(PowerShell Get-AppxPackage), launch, addon import; unvalidated → 40.
**Essentials social:** backend tables/functions complete (E3 source); launcher
UI wired (E2); no live test → 55–60. **P2P:** libdatachannel + Supabase signal
polling real; no NAT/TURN live → 51 (cap 65). **TURN:** credentials real,
accounting absent → 4. **Local servers:** provisioning real for
Paper/Purpur/Folia/Fabric; process mgmt real; no live → 60. **Cloud:**
dev-only provider; external backend absent → 15–47. **Amalgam+/Billing:**
Whop webhook real; nothing live; subscriptions empty → 20. **Supabase/Auth:**
RLS hardened, hashed node secrets, DPAPI, MS device code; rate limit unwired;
admin emails exposed → 69–73. **Updater/Installer:** updater 0; Inno installer
exists, unsigned, no clean-machine → 53. **In-game client:** 6 modules real,
HUD real, overlay social placeholders → 60/30. **Performance:** fixed 3 real
per-frame stalls this session (Home java.exe spawns/frame; detail-page
recursive scans/frame; sync mod-icon HTTP on UI thread). **Recovery:**
restore points, exclusive queue, staged installs — strongest subsystem (75).
**Offline:** launcher local features work offline; UI degrades gracefully for
networked pages (E2).

## 34. Prototype Register (user-visible placeholders)

| ID | LOCATION | TYPE | VISIBLE? | SEVERITY | SYSTEM | RELEASE IMPACT | EVID |
|---|---|---|---|---|---|---|---|
| P-01 | client_ui.cpp "Survival Squad" | fake party | yes | P1 | Client social | users see fake state | E1 |
| P-02 | client_ui.cpp "Map Coming Soon" | stub | yes | P2 | Client map | feature not delivered | E1 |
| P-03 | client_ui.cpp "Messaging coming soon" | stub | yes | P2 | Client messaging | not delivered | E1 |
| P-04 | client_ui.cpp Invite/View-Profile buttons | toast-only | yes | P2 | Client friends | misleading controls | E1 |
| P-05 | ui.cpp `NewsItem news[]` | hardcoded news | yes | P1 | Home | fake content | E1 |
| P-06 | turn_usage table | unwritten quota | no | P0 | TURN | cannot sell 80 GB claim | E1 |
| P-07 | enforce_rate_limit | unwired helper | no | P1 | Supabase | no abuse protection | E1 |
| P-08 | online_config api_url empty | absent backend | no | P0 | Cloud/Entitlements | products off | E1 |
| P-09 | cloud dev provider | dev-mode | yes | P1 | Cloud UI | honest but non-functional | E2 |
| P-10 | Home hero rotating news | static array | yes | P3 | Home | cosmetic | E1 |

## 35. Client Module Truth

All 6 native modules (Freecam, Fly, Speed, NoFall, AutoTool, KillAura) are
registered in `core/modules.cpp` with tick functions and Java-side controllers
(FreecamController, AttackController, SlotController, VelocityController in
`java-neoforge/common`; Actions in Fabric). Safe mode + param clamping exist.
`module_test.cpp` covers them. No UI-listed module lacks implementation.

## 36. HUD

Real items (FPS, Ping, Coordinates, CPS, Armor, Server TPS…) with drag/scale/
opacity/save/load fields (`client_hud2.cpp`); editor wired. Visual reference
comparison not possible here (BLOCKED — no capture).

## 37. Client ↔ Launcher Consistency

Friends: launcher writes bridge file → client parses (consistent, E2).
Party/messaging: launcher real backend, client hardcoded → **split-brain
confirmed**. Cosmetics: launcher entitlement → client bridge (consistent).
Accounts: client does not manage MS auth (launcher does) — by design.

## 38–41. Offline / Failure / Scale / Security

**Offline (55):** launcher core works offline; networked pages degrade.
**Failure injection:** not run (BLOCKED); recovery-first design gives
confidence but no evidence. **Scale:** 100 profiles / 500 mods untested;
image cache capped at 512; job queue durable; screenshot gallery bounded at
48. **Attacker pass:** client cannot self-grant Plus via Supabase (RLS);
cannot write subscriptions; cannot read others' data (RLS); CAN bypass TURN
quota (nothing enforces); CANNOT obtain node credentials (hashed + owner-gated
RPCs); archive extraction hardened (traversal/zip-bomb caps). admin emails
exposed to staff only.

## 42–43. Do Not Touch / Consolidate Later

**Do not rewrite:** wire protocol (byte-tested), DPAPI layer, Java runtime
manager, extractor, exclusive job queue, server provisioning, RLS hardening,
Whop HMAC (add live provider config only). **Consolidate later:** client
overlay social vs launcher social; diagnostics outbox ingestion; duplicate
`load_texture` vs `request_image` caches.

## 44. Top 40 Findings (ranked)

1. P0 TURN accounting absent (no writer) — cannot honor Amalgam+ quota.
2. P0 No self-updater — shipping/distribution blocker.
3. P0 Cloud backend absent — Cloud product non-deliverable.
4. P1 Fresh build unverified in this environment.
5. P1 Rate-limit helper unwired.
6. P1 Client fake party ("Survival Squad").
7. P1 Client map "Coming Soon".
8. P1 Client messaging "coming soon".
9. P1 Hardcoded Home news.
10. P1 config save non-atomic.
11. P1 No live MS account launch.
12. P1 No Bedrock live validation.
13. P1 No clean-machine matrix.
14. P1 No code signing.
15. P1 Entitlements external API absent by default; subscriptions empty.
16. P1 admin_list_users exposes emails.
17. P1 Quilt beta pin unproven.
18. P1 Zero E2E/fault-injection tests.
19. P2 ui.cpp 12.7k lines.
20. P2 Split-brain client/launcher social.
21. P2 Repo hygiene (1.5 GB hprof, NUL.obj).
22. P2 Stale docs (corrected).
23. P2 Cosmetics ownership client-side.
24. P2 Performance gains unmeasured.
25. P2 Diagnostics outbox not uploaded.
26. P2 client friend buttons toast-only.
27. P3 Home hero news static.
28. P3 Accessibility/keyboard depth unknown.
29. P3 200 % DPI unverified.
30. P3 Icon cache no retry-on-failure.
31. P2 Essentials invite token == invite_id (invite-replay surface; invites expire — acceptable but needs live abuse test).
32. P2 WebRTC bridge lacks reconnect/cleanup tests.
33. P2 Server console buffer in-memory (agent) — lost on restart.
34. P2 Node agent lacks operation timeout enforcement.
35. P2 Migration 006→007 window had broad write policies (final state safe).
36. P2 `has_unreadable_secrets` guard is good but only protects config path.
37. P3 No SELF-UPDATE → no rollback story.
38. P2 No live provider rate-limit handling test (429).
39. P2 Large-modpack (hundreds of files) install untested at scale.
40. P2 Cloud plan prices hardcoded in launcher UI (display-only; backend must own).

## 45. Top 30 Build Queue (ordered)

1. Wire `enforce_rate_limit` into all social/essentials/party RPCs; add abuse tests.
2. Authoritative TURN usage ingestion (relay/provider metrics server-side, monthly reset, quota enforcement, cutoff behavior) — unblocks P0-1.
3. Run full CTest matrix on Windows (verify current source).
4. Deploy Supabase migrations + functions to a linked project; verify 007 on snapshot; test fresh/partial migrations.
5. Build launcher self-updater (manifest, signature, staged replace, rollback).
6. Replace client placeholders with backend-bound data or hide them.
7. Replace hardcoded Home news with backend feed + offline fallback.
8. Atomic config writes (temp + rename + backup) — close P1-9.
9. Redact emails from admin_list_users.
10. Live MS account launch (owner) + record.
11. Clean-machine Fabric/Quilt/Forge/NeoForge matrix.
12. Bedrock live validation (UWP machine).
13. Code signing + installer bootstrap + SmartScreen check.
14. Fault-injection tests: download kill, disk-full, 429/500, corrupted jars, truncated config.
15. Quilt clean-room validation or drop the claim.
16. Scale pass: 100 profiles / 500 mods / large console.
17. Offline-mode audit + explicit degraded states.
18. `ui.cpp` split (documented).
19. Merge client/launcher social state or gate the client overlay.
20. Agent: operation timeout enforcement + console flush persistence.
21. Agent: jar auto-download from version manifest.
22. Performance instrumentation (frame budget guard) + measure gains.
23. Diagnostics outbox: decide upload path (documented contract only today).
24. Icon/texture cache unification (request_image vs load_texture).
25. Essentials invite replay/abuse live test.
26. WebRTC reconnect/cleanup tests.
27. 200 % DPI + accessibility pass.
28. Cloud: define deployment target; hook entitlements API; remove dev-only UI.
29. Billing: Whop production keys + customer mapping + webhook E2E.
30. Release package: sign, SBOM verify, clean install/uninstall/upgrade evidence.

## 46–49. Release Gates (binary)

**CLOSED BETA:** fresh Windows build passes 31/31 CTest (YES/NO) · live MS
account launch (YES/NO) · Supabase deployed + verified (YES/NO) · Bedrock
detect on real machine (YES/NO) · placeholders removed or gated (YES/NO).
**OPEN BETA:** adds: clean-machine Fabric/Quilt/Forge/NeoForge matrix
(YES/NO) · signed launcher/DLL (YES/NO) · TURN accounting live (YES/NO) ·
rate limits active (YES/NO) · updater v1 (YES/NO). **RELEASE CANDIDATE:**
adds: full security review (YES/NO) · 1,000-user dry-run (YES/NO) · provider/
legal review (YES/NO). **PUBLIC RELEASE:** adds: signing + SmartScreen
reputation (YES/NO) · live Cloud + billing E2E (YES/NO) · TURN quota
enforcement E2E (YES/NO) · update rollback verified (YES/NO).

## 50. Final Scorecard

Implementation 84 · Functionality 68 · Integration 55 · Reliability 65 ·
Recovery 75 · Security 69 · Desktop UX 80 · Client UX 45 · AAA Polish 48 ·
Performance 70 · Testing 45 · Maintainability 50 · Release Engineering 35

**OVERALL: 68/100 — INTERNAL ALPHA.**

## 51. Contradiction search (second pass)

- "Supabase hardened" vs "rate limiting unwired" → RESOLVED: hardening = RLS +
  hashed secrets (true); rate limiting = helper only (finding P1-6).
- "Recovery-first" vs "config save non-atomic" → RESOLVED: job/install state is
  atomic; `launcher.json` is not (P1-9).
- "Cosmetics not hardcoded" vs V1 "hardcoded" → RESOLVED: bridge-loaded;
  ownership client-derived (P2-6).
- "19 bridges" vs "Quilt uses Fabric jar" → RESOLVED: by design, documented.
- "31 CTest" vs V1 "24" → RESOLVED: suite grew; none executed here.
- TURN claim (80 GB/month) vs zero writers → RESOLVED: unenforceable today.
- No UNRESOLVED contradictions remain.

**Final check:** no secrets printed · no fake percentages · caps applied ·
BLOCKED items marked · unit vs integration separated · TURN traced · final
migration state inspected · updater searched · high scores challenged ·
V1→V2 delta included · placeholders registered.
