# Amalgam — Master Plan

Official product: **Amalgam Launcher** — https://amalgam-mc.com/

The website is already built and published. Do not create another website. Do not
rebuild the website. Do not move the launcher into Replit.

The launcher stays a **native C++17 / ImGui / Windows desktop application**.

## Ecosystem

```
amalgam-mc.com (HTTPS)
        │
        ▼
  AMALGAM ONLINE API
        │
   ┌────┴────┐
   ▼         ▼
 SUPABASE   ONLINE DATA
Auth / DB   Essentials / Plans
   │         │
   └────┬────┘
        ▼
 NATIVE C++ LAUNCHER (user's Windows PC)
```

Website and launcher share the same Supabase account but keep separate active
sessions. Never copy browser cookies into the launcher. Protect launcher refresh
tokens with Windows DPAPI/Credential Manager, never plain files, and never ship
service-role/secret credentials in the launcher.

## Core direction

One unified platform: Java, Bedrock, mods, modpacks, profiles, Java runtimes,
worlds, servers, friends, world hosting, multiplayer, downloads, backups, repair,
and future 24/7 hosting.

Core experience: DISCOVER → INSTALL → MANAGE → PLAY → HOST → INVITE → SYNC →
CREATE SERVER → optionally move to cloud.

## Main navigation

Home · Discover · Library · Downloads · Essentials · Servers · Bedrock ·
Java Manager · Settings · Account

Keep the existing dark navy/charcoal + purple accent + rounded-card design.

## Key rules

- Upgrade existing systems; do not rewrite what already works.
- Java manager, Bedrock management, local profiles, local servers, basic
  mods/modpacks, and Essentials friends/invites stay FREE.
- Local servers run on the user's hardware. Cloud is Amalgam infrastructure and
  is paid. Clearly distinguish the two.
- Unlimited direct Essentials P2P. STUN before TURN. TURN is metered by GB and is
  fallback only. Never ship permanent TURN credentials. Never sell unlimited
  relay/storage/backups/RAM.
- Backend is authoritative for plan entitlements and TURN/cloud quotas. Never
  trust limits supplied by the launcher.
- Prefer provider downloads (CurseForge/Modrinth, Paper/Fabric/Purpur APIs) over
  self-hosting duplicate files. Prefer temporary synced profiles over destructive
  sync. Do not blindly copy client-only mods into servers.
- Keep official Minecraft Launcher fallback until the Amalgam AppID is approved.
  Never bypass Minecraft ownership/authentication.
- Local functionality must survive backend outages. Ads subsidize free users and
  are not the business foundation. Cloud/Plus sell convenience/compute.

## Server software APIs

- Fabric: https://meta.fabricmc.net/ (versions, loader, installer, server runtime)
- Paper/Folia/Velocity: https://fill.papermc.io/v3/ (versions, builds, checksums)
- Purpur: https://api.purpurmc.org/ (versions, builds, direct downloads)
- Quilt: https://meta.quiltmc.org/ (versions, loader, installer)

ServerRuntimeProvider interface: GetSupportedVersions, GetBuilds,
GetLatestStableBuild, GetDownloadURL, DownloadRuntime, VerifyRuntime,
InstallRuntime. No Gradle required merely to obtain server runtimes.

## Beta order

1. Finish local V2 (launcher, profiles, Discover, Java manager, Bedrock, local
   servers, downloads, repair, backups).
2. Accounts (Supabase + website + launcher login + API bridge).
3. Essentials (friends, invites, presence, sessions, direct P2P, STUN, TURN).
4. Sync (compatibility, manifest compare, provider downloads, temp profiles).
5. Monetization (ads, Amalgam+, TURN quotas/top-ups, entitlements).
6. Cloud (control plane, node agent, containers, provisioning, console, files,
   backups, metrics) — start with 10–25 servers before broad launch.

Measure Essentials/cloud beta telemetry before locking pricing.

## Pricing targets (provisional)

- FREE: $0 — full launcher, all main local features, local servers, Essentials
  friends/invites, unlimited direct P2P, 5 GB TURN/month, ads enabled.
- AMALGAM+: $4.99/mo — no ads, 25 GB TURN, higher online limits.
- CLOUD 4: $9.99 — 4 GB RAM, 25 GB storage, 50 GB TURN.
- CLOUD 8: $16.99 — 8 GB RAM, 50 GB storage, 100 GB TURN.
- CLOUD 12: $24.99 — 12 GB RAM, 100 GB storage, 150 GB TURN.
- Optional add-ons: Always-On compute, extra TURN/storage/backups, extra slots.

TURN top-ups: +25 GB $2.99, +50 GB $4.99, +100 GB $8.99. Monthly quota does not
roll over. No unlimited relay.

## Business discipline

Revenue is not profit. Meter every resource that costs recurring money, keep
tax/emergency/infrastructure reserves, and keep every plan profitable under heavy
legitimate use. Keep the control plane separate from untrusted Minecraft workload
nodes. Maintain internal usage/cost monitoring and alerts (TURN 70/85/95%, node
RAM 80/90%, CPU 80%+, storage 80/90%, bandwidth/signup/provisioning/payment-failure
spikes, margin below target).

## Final goal

ONE ACCOUNT · ONE LAUNCHER · ONE MINECRAFT ECOSYSTEM — discover, install, manage,
play, host, invite, sync, create servers, and host them 24/7 when needed.
