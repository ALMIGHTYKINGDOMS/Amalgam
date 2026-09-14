# Full Project Audit — 2026-08-21 (consolidated)

**Scope:** 100% of the checkout: native client DLL (`cpp/src`), launcher
(`cpp/launcher`), Java bridges (`java`, `java-neoforge`, `java-forge`,
`java-forge-legacy`, `java-forge-1.12.2`), Supabase control plane
(`supabase`), runtime agent (`tools/runtime-agent`), tooling, docs, and
packaging.

**Method:** direct source verification against the current tree, plus the
available executable checks. Native MSVC rebuild and live Supabase
deployment are **not** possible in this shell (no CMake/MSVC/Ninja, no
Supabase CLI), so those are reported as unexecuted rather than assumed.

---

## 1. Headline: earlier audits are stale

`docs/client-launcher-audit-2026-08-21.md` and `docs/deep-audit-2026-08-21.md`
were written before migrations `202608210003/004/005/006/007` and the 21
social Edge Functions were added. Their central claims are now **wrong**:

| Claim in stale audits | Actual current state |
|---|---|
| `profiles` table MISSING | ✅ `202608210003_launcher_tables.sql` creates it (RLS: owner-only) |
| `bedrock_profiles` MISSING | ✅ created in `...003` |
| `subscriptions` MISSING | ✅ created in `...003` |
| `turn_usage` MISSING | ✅ created in `...003` |
| `nodes` MISSING | ✅ created in `...003` (launcher-side, distinct from `hosting_nodes`) |
| `servers` missing `current_players`/`last_ping` | ✅ added in `...003`; `last_started`/`assigned_at` exist via runtime migration |
| 21 social Edge Functions MISSING | ✅ all 21 present in `supabase/functions/` |
| `account_activity` realtime table MISSING | ✅ created in `...004` |
| plaintext `join_token` column still in DB | ✅ `...002` drops it; `...007` drops `node_secret` too |
| `node_secret` stored plaintext | ✅ `...005` hashes it; `...007` drops the plaintext column |
| no rate limiting on RPCs | ✅ `...007` adds atomic advisory-lock rate limiting |

These two docs are now marked superseded; this document is the current
source of truth.

## 2. What was verified as correct (with evidence)

### Supabase control plane
- **17 migrations, 23 Edge Functions** — `node tools/verify-supabase.mjs`
  passes ("Supabase source verification passed: 17 migrations, 23 Edge
  Functions"). This is a local source check, not a live-project check.
- All 8 realtime topics the launcher subscribes to (`servers`, `profiles`,
  `bedrock_profiles`, `account_activity`, `messages`, `user_presence`,
  `friend_requests`, `party_members`) now have backing tables.
- Every Edge Function name the C++ launcher calls (`get_friends`,
  `send_friend_request`, `search_users`, `get_conversations`,
  `create_party`, `update_presence`, `get-turn-credentials`, …) matches a
  deployed function directory exactly (`supabase.cpp` ~lines 2689–3025).
- Auth reads use `GET /auth/v1/user`, writes use `PUT`; PostgREST upserts
  pass `on_conflict`; the realtime layer is bounded HTTP polling with
  unsubscribe/shutdown (`supabase.cpp`).
- Rate limiting, node-secret hashing, restricted `staff_roles`-style write
  paths, and atomic social RPCs are implemented in `...007`.

### Native launcher (`cpp/launcher`)
- **net.cpp**: HTTPS-only (`url_allowed` rejects non-https and embedded
  credentials), 64 MB response cap / 2 GB download cap, SHA-1 and SHA-256
  verification, resume from `.part` with 206 handling, 4xx treated as
  non-retryable, exponential backoff on transient failures. Reviewed
  directly; looks solid.
- **extract.cpp**: entry scan, `..` traversal rejection (`unsafe_entry`),
  4 GB extracted-bytes cap, 20k entry cap, staged extraction to temp dir.
- **import_pack.cpp**: rejects `..` segments and empty/dot filenames,
  runtime-owned/launcher-metadata entries excluded from review and update
  payloads.
- Modules (`cpp/src/modules/core_modules.cpp`) are **real implementations**,
  not stubs: freecam (position + yaw/pitch via `ACT_SET_POS`), fly
  (`ACT_SET_VELOCITY`), speed (grounded velocity boost), nofall
  (`ACT_MOVE_ON_GROUND` past fall threshold), autotool (`ACT_SWAP_SLOT` on
  break), killaura (nearest hostile, kind==1 only, attack cooldown). All
  express actions through vanilla packets at tick start per the whitelist.
- Wire protocol (`cpp/src/core/protocol.h`): `Action` = 36 bytes with
  `static_assert(sizeof == 36)`; V1/V2 snapshot parsing bounds-checks counts
  against `len` before copying; V2 entities supersede legacy hostiles.
  Java-side byte parity is covered by
  `java/common/src/test/java/amalgam/bridge/ProtocolTest.java` plus
  `cpp/tests/protocol_test.cpp`.
- Secrets: none found in source. Only test-fixture strings (e.g.
  `modrinth-test-secret` in `config_test.cpp`) and one vendored test key in
  `cpp/vendor/libdatachannel/test/connectivity.cpp`. Launcher credentials
  are DPAPI-protected in `launcher.json`, gitignored.
- **19 runtime bridge jars** present in `cpp/build/bridges/` (9 Fabric + 6
  NeoForge + 4 Forge), matching the advertised package inventory.

### Runtime agent (`tools/runtime-agent`)
- **7/7 Node tests pass** (`npm test`): config validation, metrics parsing,
  event batch flush/requeue, restore path safety, backup rotation.
- Hardening present: HTTPS-only jar downloads with `.part` staging, no shell
  interpolation, shutdown stops managed processes and flushes events,
  console stream preserves partial lines, backup rotation.

### Java bridges
- 9 Fabric, 6 NeoForge, 4 Forge projects build per AGENTS.md records (not
  re-run here — no Gradle/JDK toolchain in this shell). Jars in
  `cpp/build/bridges/` match those counts.

## 3. What remains (external gates — cannot be completed in this checkout)

Unchanged from `docs/release-gates.md` and `docs/production-audit-2026-08-21.md`:

1. **Native rebuild + full CTest matrix on Windows** (MSVC/Ninja) — the
   last green build in `cpp/build/` predates the latest source edits.
2. **Live Supabase deployment** — install/authenticate the Supabase CLI,
   apply all 17 migrations, deploy the 23 functions, verify `...007` on a
   DB snapshot.
3. **Edge Function typecheck** with Deno / Supabase CLI.
4. **Live Microsoft account launch** (owner action), **Bedrock UWP**
   detection/launch/import, **clean-machine launch matrix**.
5. **Code signing** + installer bootstrap + SmartScreen verification.
6. **Whop production** signing secret + customer mappings; **AI art
   generation** with a configured provider; provider/legal/support review.

## 4. New findings from this audit

### 4.1 Repo hygiene (low severity, easy fix)
- `java_pid19316.hprof` (770 MB) and `java_pid22008.hprof` (771 MB) — JVM
  heap dumps at the repo root, ~1.5 GB of junk. Gitignored (`*.hprof`) but
  waste disk; safe to delete.
- `NUL.obj` (443 KB) — Windows redirect artifact at the repo root; safe to
  delete.
- `imgui.ini` at root is gitignored; `amalgam_diagnostics_test.sqlite3` is
  an empty stray directory.
- Root copies of `amalgam-forge-1.18.2.jar` / `1.19.2.jar` duplicate the
  jars under `cpp/build/bridges/`.

### 4.2 Documentation drift (low severity)
- `docs/project-status.md` mixes "30 CTest suites" and "31 CTest suites"
  (the suite grew to 31 with `amalgam_cloud_hosting_test`).
- README says "All 30 CTest checks" while the current count is 31.
- The stale audit docs (see §1) needed correcting — done via superseded
  notices.

### 4.3 Remaining product gaps (documented, not hidden)
- Quilt is pinned to `quilt-loader 0.20.0-beta.9` and not clean-proven.
- Bedrock adapter has automated coverage but no live validation on this
  machine (no UWP package installed).
- Remote diagnostics ingestion is a documented HTTPS contract only; the
  local SQLite outbox intentionally does not upload.
- Full world replay is out of scope by design (redacted telemetry replay
  only).

## 5. Scores

| Component | Score | Basis |
|---|---:|---|
| Native protocol / pipeline / modules | 9/10 | static asserts + Java parity tests; code reviewed |
| Launcher network & archive safety | 9/10 | reviewed; unexecuted rebuild |
| Supabase schema + functions (source) | 9/10 | verifier passes; stale audits corrected |
| Runtime agent | 8/10 | 7/7 tests; live Supabase run missing |
| Java bridge coverage | 8/10 | 19 jars present; clean-room game validation missing |
| Release readiness | ~60% | blocked on external gates (§3), not missing code |

**Bottom line:** the codebase is substantially complete and internally
consistent; the remaining work is execution of external release gates
(account, clean machine, signing, live backend), plus the low-severity
cleanup items in §4.
