# Beta Backend Deployment

Project ref: `nnrrmvaxnoknthpwvttt`.

The launcher must never ship a Supabase service-role key. Apply and verify the
backend from an authenticated operator environment only.

## Local preflight

```bash
node tools/verify-supabase.mjs
```

This checks migration delimiters, required migration/function files, config
sections, and deployment safety. It does **not** prove that the live project is
updated.

## Deploy migrations and functions

```bash
bash tools/deploy-supabase.sh verify
bash tools/deploy-supabase.sh migrations
bash tools/deploy-supabase.sh functions
```

The migration command runs one fail-fast `supabase db push --linked`, which
applies every file in `supabase/migrations/` in filename order, including the
runtime, data collection, launcher, social, security, upgrade, and production
hardening migrations. The function command discovers every function directory;
JWT behavior comes from `supabase/config.toml`.

The only intentionally public function is `whop-webhook`. Configure these
Supabase secrets before deploying it:

- `WHOP_WEBHOOK_SECRET`
- `SUPABASE_SERVICE_ROLE_KEY`
- optional `AMALGAM_ALLOWED_ORIGIN` for browser callers

The authenticated `curseforge-catalog` function keeps the shared provider key
out of every launcher package. Configure this Edge Function secret as well:

- `CURSEFORGE_API_KEY`

The function accepts only the small set of CurseForge Minecraft catalog paths
used by Amalgam, validates pagination/content classes, caps response size, and
rate-limits each signed-in Amalgam account. Do not put this key in a release
`launcher.json`; a personal key remains a developer-only override.

Whop customer-to-Amalgam mappings must be inserted into
`public.whop_customer_mappings` through a trusted operator workflow. A Whop
customer ID is not assumed to be a Supabase UUID.

## Runtime node enrollment

Provision a `hosting_nodes` row through an authenticated launcher/API flow and
return the node ID plus one-time secret to the node operator. Migration
`202608210007_production_hardening.sql` hashes the secret and removes the
plaintext column. The runtime agent should use:

```text
SUPABASE_URL
SUPABASE_ANON_KEY
SUPABASE_ACCESS_TOKEN (owner session, when enrolling)
AMALGAM_NODE_ID
AMALGAM_NODE_SECRET
```

A service integration may operate a pre-provisioned node, but the agent refuses
to auto-register nodes with a service-role key.

## Operator verification

After deployment, verify from the Supabase dashboard or SQL editor:

1. Migration history includes every local migration through `202608210007`.
2. `hosting_nodes` no longer has a plaintext `node_secret` column and has a
   populated `hashed_secret` for existing nodes.
3. `subscriptions`, social writes, conversation creation, party mutations, and
   runtime RPCs reject unauthorized users.
4. Edge Function logs show JWT verification for authenticated functions.
5. Whop duplicate webhook delivery returns a duplicate acknowledgement.
6. A pre-provisioned runtime node registers/heartbeats, claims one operation,
   streams console lines, reports telemetry, and goes offline cleanly.

Never include service keys, user access tokens, webhook secrets, raw logs, or
private feedback diagnostics in a release archive.
