# AMALGAM V3 — LIVE TEST PROCEDURES (2026-08-21)

Every test below could not be executed in this environment. Each section is
an executable procedure with pass criteria, ready to run on the right
hardware/credentials. Status is tracked in `docs/verification-report-2026-08-21.md`.

---

## 1. SUPABASE DEPLOYMENT

Prepared:
- `tools/_pgcheck/precheck.mjs` — read-only live checks (applied migrations,
  RLS policies on subscriptions/turn_usage/turn_usage_events, function
  presence, admin email masking, row counts). Run:
  `cd tools/_pgcheck && node precheck.mjs <path-to-pooler-url>`
- Precheck blocked: the workspace `supabase/.temp/pooler-url` contains no
  password, and no `SUPABASE_ACCESS_TOKEN` is present.

To deploy (requires one of):
- **CLI (preferred):**
  ```
  npm i -g supabase            # or npx supabase
  supabase login               # paste personal access token
  supabase link --project-ref nnrrmvaxnoknthpwvttt
  supabase migration list      # confirm 18 local / <n> applied
  supabase db push             # apply missing migrations in order
  supabase functions deploy    # all 23 functions
  ```
- **Direct SQL fallback** (only if you have the DB password): apply the
  missing migration(s) in filename order via psql/`pg`, then record them in
  `supabase_migrations.schema_migrations(version, name, statements)`.

Pass criteria after deploy:
- `precheck.mjs` shows all 18 versions applied.
- `verify-production-schema.mjs` still 65/65 against the live project.
- RLS: no `authenticated` INSERT/UPDATE/DELETE policy on `subscriptions`,
  `turn_usage`, or `turn_usage_events`; `ingest_turn_usage` granted only to
  `service_role`; `admin_list_users` body has no `email` column.
- Migration 008 items present: rate-limit triggers, `plan_quotas` rows
  (`free` 1 GiB, `amalgam_plus` 80 GiB), `turn_usage_events` unique partial
  index on `event_id`.

## 2. EDGE FUNCTIONS — AUTH + RATE LIMIT TESTS

With an anon key and two test users (A, B):
1. Normal volume: 20 friend requests A→B → all succeed.
2. Burst: 60 requests in <5s → requests beyond the per-user window return
   429 (function returns `{error:"rate limit exceeded"}` with status 429).
3. Independent users: a second user C is unaffected by A's burst.
4. Expired window: wait for the window to elapse → A can request again.
5. Read vs write: read functions (`get_friends`, `get_messages`) have
   separate, higher limits; verify they are not tripped by write bursts.
6. Auth: unauthenticated calls to any `verify_jwt=true` function are
   rejected (401) before logic runs.

Pass: only write/spam operations rate-limit; no 5xx; reads unaffected.

## 3. TURN LIVE E2E

Requires a live TURN provider (Cloudflare or compatible) + the ingestion
pipeline credential.
1. Configure the provider to emit per-user relay byte events.
2. Ingestion job (external) calls `ingest_turn_usage(user_id, bytes,
   'relay', event_id)` with the service role for each provider event.
3. Establish a real relayed session; observe `turn_usage.used_bytes`
   increase by exactly the relayed bytes (P2P traffic must not appear).
4. Replay the same `event_id` → `{ingested:false, duplicate:true}`, no
   double count.
5. Drive usage to 75% / 90% / 100% of the allowance (1 GiB free plan):
   launcher shows warning states at 75% and 90%.
6. At 100%: `get-turn-credentials` returns 429 `TURN_QUOTA_EXHAUSTED`;
   direct P2P still works; unrelated local features unaffected.
7. After `period_end` passes: `rollover_turn_period` resets `used_bytes` to
   0 and advances the period; new credentials are issued again.

Pass: byte-accurate accounting, no double count, cutoff + reset verified.

## 4. MICROSOFT / JAVA LIVE TEST

With a legitimate Microsoft Minecraft account, per loader record:
Minecraft version, loader version, Java selected, launch result, exit code,
logs. Reach the actual Minecraft menu/world for: Vanilla, Fabric, Forge,
NeoForge. Test Quilt separately and keep it labeled experimental unless it
passes cleanly twice.

## 5. CLEAN MACHINE

On a Windows VM with no Amalgam config, managed Java, or dev tools:
installer → launch → sign-in → Java acquisition → profile creation →
Minecraft download → launch → restart persistence → update check →
uninstall. Verify worlds/profiles survive uninstall (unless data removal is
explicitly chosen).

## 6. BEDROCK

On a machine with Minecraft for Windows: detect install → launch → profile
create/manage → import `.mcpack` / `.mcaddon` / `.mcworld` → backup world →
restore world. Record every failure; imports must never corrupt an existing
world without recovery.

## 7. ESSENTIALS (TWO REAL NETWORKS)

Host on network 1, join from network 2:
- A: direct P2P (no relay) — play session works.
- B: forced TURN/relay — play session works, usage counted.
- C: host disconnect → guest gets clean disconnect, both clean up.
- D: guest disconnect → host stays up, cleanup.
- E: reconnect after network blip.
- F: expired/replayed invite → rejected; single-use invite consumed.
Verify cleanup of sessions, signaling rows, DataChannels, sockets, threads,
and temporary state (no leaked sessions after either side disconnects).

## 8. RELEASE SIGNING

- Generate a signing keypair OUTSIDE the repository; store the private key
  in the CI secret store / secure vault only. Never commit it.
- Sign: launcher exe, `amalgam.dll`, installer, and update payloads
  (RSA-SHA256 over payload bytes — matches `verify_payload_signature`).
- Ship the PUBLIC key PEM to production release config
  (`set_signing_public_key`) and to the manifest.
- Verify signatures after packaging (`signtool verify` for Authenticode;
  updater path verified by `amalgam_updater_test`).

## 9. UPDATER LIVE TEST

Build old + new signed builds and a production-like manifest.
1. old → new: check shows Available, download verifies, helper replaces,
   relaunch succeeds, version advanced.
2. Tampered package → hash mismatch → rejected, old build intact.
3. Wrong signature → signature verification fails → rejected.
4. Bad hash in manifest → rejected.
5. Locked executable (launcher still running) → helper waits, then replaces.
6. Interrupted update → last-known-good restored on next launch
   (`rollback_update` path).
Anti-rollback: manifest naming an older version is refused
(`should_apply`), and a manifest change between check and install is caught
at install time.

## 10. WHOP

Test subscription lifecycle: checkout → webhook → verified subscription →
entitlement → launcher refresh. Then cancel → webhook → entitlement
removal. Deliver the same webhook twice → second delivery is idempotent
(no double grant/revoke).

## 11. CLOUD

Determination: the launcher Cloud page is a read-only landing linking to the
Amalgam website; no purchase or deploy exists in the launcher, and pricing
is labeled "for reference". The control plane / scheduler / provisioning
backend is external (website). Status: **GATED** — do not mark Cloud live
until the website backend is verified; keep paid deploy disabled (it
already is, by construction).
If/when the backend exists, run: purchase/entitlement → provision → start →
connect → console → files → backup → restore → stop → delete, with node
agent operation timeouts and console persistence verified.
