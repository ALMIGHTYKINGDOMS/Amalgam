# Free-First Roadmap

Amalgam should remain usable without paid infrastructure.

## Available On Free Services

- Local Java, Forge, Fabric, NeoForge, Quilt, and Bedrock management.
- Local server creation, process control, console, backups, and metrics.
- Modrinth browsing and installation.
- CurseForge browsing when a free CurseForge API key is configured.
- Bedrock add-on discovery and import through CurseForge.
- Supabase free-tier authentication, Essentials session control, friends/presence,
  project drafts, media metadata, moderation records, and RLS-backed publishing.
- Staff review and one-time project approval through the publishing migration and RPC functions.
- Local client HUD, telemetry, replay, screenshots, and performance tools.

## Coming Soon

These remain explicitly unavailable until paid infrastructure is provisioned:

- Amalgam Cloud hosted servers and billing plans.
- Public relay/tunnel networking and TURN infrastructure. Essentials signaling and
  direct host coordination are free; only relay infrastructure is deferred.
- CDN-backed project media delivery at scale.
- Paid monitoring, DDoS protection, global server regions, and 24/7 hosted uptime.
- Payment processing, subscriptions, invoices, and automatic cloud provisioning.

The launcher must not silently simulate paid infrastructure as operational. UI entry points should show `Coming Soon` and leave local alternatives available.
