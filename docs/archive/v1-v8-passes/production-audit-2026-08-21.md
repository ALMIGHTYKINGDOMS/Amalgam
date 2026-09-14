# Production Audit — 2026-08-21

## Scope

This audit follows the earlier over-optimistic completion reports and records
what is actually present in the checkout after the production-hardening pass.

## Implemented locally

### Supabase

- Added migration `202608210007_production_hardening.sql`.
- Runtime node secrets are backfilled into `hashed_secret`, protected by a
  unique partial index, and the plaintext `node_secret` column is dropped.
- Node registration now requires an authenticated owner; node-scoped runtime
  RPCs verify the hash and validate resource values.
- Added atomic rate-limit enforcement using an advisory transaction lock.
- Removed broad authenticated write paths for subscriptions, friendships,
  friend requests, conversations, messages, party mutations, and audit/activity
  records.
- Added atomic RPCs for friend requests, friend acceptance/rejection/removal,
  conversation creation/message append/read state, and party lifecycle.
- Added Whop customer mapping and webhook idempotency tables.

### Runtime agent

- Startup now launches heartbeat, operation polling, console flushing, telemetry,
  and event collection concurrently.
- Provisioned nodes use `node_id + node_secret`; owner access/refresh tokens are
  supported for enrollment and long-running sessions.
- Shutdown stops managed Minecraft processes, flushes event data, and marks the
  node offline on a best-effort basis.
- Event batches are requeued after terminal failures instead of being silently
  discarded.
- Jar downloads require HTTPS, use staged `.part` files, avoid shell command
  interpolation, and optionally verify SHA-256.
- Restore paths are constrained to the server backup directory.
- Console stream chunks preserve partial lines across process output events.
- Added seven Node tests covering config, metrics, event batching/requeue,
  lifecycle path safety, and backup rotation.

### Edge Functions and Whop

- Added bounded JSON parsing, UUID/length/range/enum validation helpers, and
  rate-limit helper support.
- Hardened high-risk social writes to use database RPCs.
- Whop webhook now supports timestamped HMAC verification, replay-window checks,
  duplicate-event protection, explicit customer mapping, safe UUID handling, and
  non-sensitive error responses.

### C++ launcher

- Supabase Auth user reads now use `GET`; user updates use `PUT`.
- PostgREST upserts now include `on_conflict` when requested.
- Replaced the explicit realtime no-op with a bounded HTTP polling subscription
  that emits changed rows and supports unsubscribe/shutdown.
- AI-generated loader-specific profile art and onboarding art are used when
  locally generated assets exist, with bundled fallbacks preserved.

### Deployment and art

- Replaced the unsafe deployment loop with fail-fast migration/function deploys.
- Added `tools/verify-supabase.mjs` for local source verification.
- Added `tools/generate-art.ps1`, `docs/branding/ai-art-prompts.md`, and
  `docs/branding/ai-art-manifest.json`.

## Verification performed

| Check | Result |
|---|---|
| Bash deployment-script syntax | PASS |
| Supabase source verifier | PASS: 17 migrations, 23 Edge Functions |
| Runtime JavaScript syntax | PASS |
| Runtime Node tests | PASS: 7/7 |
| PowerShell art-script presence check | PASS |
| Supabase CLI live verification | NOT RUN: CLI unavailable |
| Deno/Edge Function typecheck | NOT RUN: Deno unavailable |
| Native C++ rebuild | NOT RUN: CMake/MSVC/Ninja unavailable in this shell |
| Native CTest execution | NOT RUN: CTest unavailable in this shell |
| Live Supabase migration/function deployment | NOT RUN: requires explicit operator credentials/authorization |
| AI raster generation | NOT RUN: requires configured image provider/API access |

## Remaining real gates

1. Install/authenticate the Supabase CLI and run the deployment script against
   the linked project.
2. Verify migration `202608210007` on a database snapshot before production,
   especially existing subscription duplicates and the pgcrypto extension schema.
3. Run Edge Function typecheck/tests with Deno or the Supabase CLI.
4. Rebuild the C++ launcher with the documented MSVC toolchain and run the full
   CTest matrix; the realtime polling code must be exercised on Windows.
5. Provision a real hosting node, run the agent against the deployed project,
   claim a test operation, stream console/telemetry, and verify clean shutdown.
6. Configure Whop’s actual production signing secret/event format and populate
   customer mappings before accepting billing events.
7. Run `tools/generate-art.ps1` with a configured image provider, review every
   image for text/watermarks/copyright/cropping, then package approved assets.
8. Complete the independent Microsoft account, Bedrock UWP, clean-machine,
   signing/installer, provider-approval, policy, and legal release gates.

## Honest status

The checkout is now materially safer and more internally testable, but it is
**not yet honestly claimable as live production-ready** until the external
Supabase, Windows toolchain, runtime-node, Whop, AI-provider, account, and
clean-machine gates above are executed and recorded.
