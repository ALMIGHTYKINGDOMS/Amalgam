# Amalgam Free Beta Release Contract

## Beta Scope

The beta is local-first and free to use. It includes local profile management,
Java/Bedrock launch flows, local server management, mod and add-on discovery,
client HUD/telemetry, screenshots, performance controls, Supabase account
features, and first-party project review/publishing after the Supabase migration
is applied.

## Disabled Until Later

Cloud hosting, billing, subscriptions, hosted uptime, relay/TURN networking,
DDoS protection, global regions, CDN delivery, and automatic cloud provisioning
are not beta features. Their UI must remain labeled `Coming Soon` and must not
report fake online, joined, synced, or provisioned states.

Essentials Supabase session control, invites, presence, and manifests can run on
the free tier. Public relay/TURN fallback remains deferred. Essentials should
only report a session online after a real host process and direct connection are
verified.

## Deployment Checklist

1. Apply all migrations in `supabase/migrations/` in filename order, including
   `202608180001_essentials_rpc_functions.sql`.
2. Configure the free-tier Supabase project URL and anon key in launcher config.
3. Add approved moderator/admin user IDs to `public.staff_roles`.
4. Deploy and test the required social/publishing Edge Functions.
5. Create the private `project-artifacts` and `project-media` storage buckets.
6. Run the launcher smoke tests on a clean Windows machine.
7. Validate at least one supported Java profile and one Bedrock profile manually.
8. Publish a beta build with an explicit version, release notes, and rollback copy.

The launcher should not be advertised as an official public beta until the
deployment checklist is complete.
