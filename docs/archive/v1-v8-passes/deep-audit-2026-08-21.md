# Amalgam + Supabase Deep Audit — August 21, 2026

> **⚠️ SUPERSEDED** — This document was written before migrations
> `202608210003`–`202608210007` were added. The "MISSING" tables
> (`profiles`, `bedrock_profiles`, `subscriptions`, `turn_usage`, `nodes`),
> the 21 social Edge Functions, the `servers` columns, node-secret hashing,
> rate limiting, and the Whop webhook all exist in the current tree. The
> security and runtime-agent hardening sections (hashed secrets, rate
> limits, backup rotation, staged jars) were implemented in the
> production-hardening pass. See `docs/audit-2026-08-21-full.md` for the
> current consolidated audit; the priority list below is outdated.

## Executive Summary

The Supabase control plane covers **Essentials sessions, project publishing, server addresses, beta feedback, social blocks, and admin directory** well. However, the C++ launcher expects **27+ tables and 21+ Edge Functions** that have **no corresponding Supabase migrations**. The social features, profiles, subscriptions, and Bedrock support are entirely frontend-only with no backend backing.

---

## 1. CRITICAL — Missing Tables (C++ Launcher Crashes or Silently Fails)

These tables are directly queried by `SupabaseClient::select()` or `SupabaseManager` methods. If they don't exist, PostgREST returns 404/400 errors that the C++ code often swallows silently.

### 1.1 `profiles` (Java Edition profiles)
**Queried by:** `SupabaseManager::get_profiles()`, `create_profile()`, `update_profile()`, `delete_profile()` (lines 1630-1791)
**Status:** ❌ MISSING — no migration creates this table
**Impact:** Profile sync, launcher settings backup, cloud profile storage all fail silently
**Expected columns:** `id`, `user_id`, `name`, `minecraft_version`, `loader`, `loader_version`, `java_path`, `memory_mb`, `mods[]`, `resource_packs[]`, `data_packs[]`, `settings{}`, `created_at`, `updated_at`, `last_played_at`

### 1.2 `nodes` (Launcher-side node tracking)
**Queried by:** `SupabaseManager::get_nodes()`, `register_node()`, `update_node()`, `deregister_node()` (lines 1867-2020)
**Status:** ❌ MISSING — we created `hosting_nodes` for the runtime agent, but the launcher uses `nodes`
**Impact:** Node registration, health monitoring, server-node assignment all fail
**Note:** The launcher `nodes` table is simpler than `hosting_nodes`. Consider unifying or creating both.

### 1.3 `bedrock_profiles`
**Queried by:** `SupabaseManager::get_bedrock_profiles()`, `create_bedrock_profile()`, `update_bedrock_profile()`, `delete_bedrock_profile()` (lines 2381-2600)
**Status:** ❌ MISSING
**Impact:** Bedrock profile sync fails entirely
**Expected columns:** `id`, `user_id`, `name`, `minecraft_version`, `banner_path`, `icon_path`, `created`, `last_played`, `last_played_ts`, `favorite`, `group`, `packs` (jsonb), `worlds` (jsonb)

### 1.4 `subscriptions` (Whop-synced billing)
**Queried by:** `SupabaseManager::get_subscription()`, `get_all_subscriptions()` (lines 2270-2340)
**Status:** ❌ MISSING
**Impact:** Amalgam+ plan detection, entitlement checks, cloud hosting limits all fail
**Expected columns:** `id`, `user_id`, `plan_id`, `plan_label`, `status`, `amount`, `currency`, `current_period_start`, `current_period_end`, `created_at`, `cancelled_at`, `provider`, `external_id`

### 1.5 `turn_usage`
**Queried by:** `SupabaseManager::get_turn_usage()` (line 2360+)
**Status:** ❌ MISSING
**Impact:** TURN relay quota display and enforcement fail
**Expected columns:** `user_id`, `used_bytes`, `monthly_bytes`, `period_start`, `period_end`

---

## 2. CRITICAL — Missing Edge Functions (Social Features Non-Functional)

The C++ launcher calls 20 Edge Functions for social features. Only `get-turn-credentials` exists. The launcher silently returns empty arrays for all social queries.

### 2.1 Friend System (6 functions)
| Function | Status | Purpose |
|---|---|---|
| `get_friends` | ❌ MISSING | List accepted friendships |
| `get_friend_requests` | ❌ MISSING | List pending inbound requests |
| `send_friend_request` | ❌ MISSING | Create a friend request |
| `accept_friend_request` | ❌ MISSING | Accept and create bidirectional friendship |
| `reject_friend_request` | ❌ MISSING | Reject a pending request |
| `remove_friend` | ❌ MISSING | Remove a friendship |

**Required backing tables:** `friendships`, `friend_requests`

### 2.2 Messaging System (5 functions)
| Function | Status | Purpose |
|---|---|---|
| `get_conversations` | ❌ MISSING | List DM threads |
| `get_messages` | ❌ MISSING | Fetch messages in a conversation |
| `send_message` | ❌ MISSING | Send a message |
| `mark_messages_read` | ❌ MISSING | Mark conversation as read |
| `get_or_create_conversation` | ❌ MISSING | Open or create a DM thread |

**Required backing tables:** `conversations`, `conversation_participants`, `messages`

### 2.3 Party System (5 functions)
| Function | Status | Purpose |
|---|---|---|
| `get_parties` | ❌ MISSING | List user's parties |
| `create_party` | ❌ MISSING | Create a new party |
| `join_party` | ❌ MISSING | Join via invite code |
| `leave_party` | ❌ MISSING | Leave a party |
| `disband_party` | ❌ MISSING | Delete party (owner only) |
| `get_party_members` | ❌ MISSING | List party members |

**Required backing tables:** `parties`, `party_members`

### 2.4 User Discovery & Presence (4 functions)
| Function | Status | Purpose |
|---|---|---|
| `search_users` | ❌ MISSING | Search public profiles |
| `update_presence` | ❌ MISSING | Set online/offline/in_game status |
| `get_user_presence` | ❌ MISSING | Get a user's presence |
| `get_friends_presence` | ❌ MISSING | Batch presence for friends list |

**Required backing tables:** `user_presence`, `public_profiles`

---

## 3. HIGH — Missing `servers` Table Columns

The C++ launcher's `ServerManager` uses columns on the `servers` table that don't exist in migration `202608170005`:

| Column | Type | Status | Impact |
|---|---|---|---|
| `node_id` | uuid FK | ✅ Added in runtime migration | Server-node assignment works |
| `instance_id` | uuid FK | ✅ Added in runtime migration | Links to server_instances |
| `template_id` | uuid FK | ✅ Added in runtime migration | Template-based creation |
| `status` | text | ✅ Added in runtime migration | Server lifecycle tracking |
| `current_players` | integer | ❌ MISSING | Player count display |
| `node_id` (on servers) | uuid | ✅ Added | Assignment works |

**Missing from `servers` that the launcher writes:**
- `current_players` (integer, default 0)
- `last_ping` (timestamptz)
- `last_started` (timestamptz)
- `assigned_at` (timestamptz)

---

## 4. HIGH — Security Gaps

### 4.1 `node_secret` stored in plaintext
The `hosting_nodes.node_secret` column stores the pre-shared key in plaintext. Anyone with database read access (staff, SQL injection) can impersonate any node.
**Recommendation:** Store only a SHA-256 hash. The agent sends the secret, the RPC hashes it and compares.

### 4.2 No rate limiting on RPCs
All RPCs (`create_session`, `join_session`, `send_session_invite`, etc.) have no rate limiting. A malicious client can:
- Spam session creation
- Flood invite requests
- Brute-force join tokens (though they're SHA-256 hashed)
**Recommendation:** Add `pg_blocking` or a rate-limit middleware table.

### 4.3 `is_project_staff()` is a security-definer function callable by anyone
The function itself is fine (returns boolean), but it's used as a gate in RLS policies. If a user somehow gets a row in `staff_roles`, they can read ALL projects, ALL feedback, ALL users.
**Recommendation:** Ensure `staff_roles` inserts can only be done by existing staff (add a trigger or restrict INSERT).

### 4.4 `admin_list_users()` exposes email addresses
The admin user directory returns `email` for all users. If a staff account is compromised, all user emails are exposed.
**Recommendation:** Consider returning only `id`, `username`, `display_name`, `avatar_url` for the admin directory.

### 4.5 No RLS on `essentials_manifest_mods` for host writes
The manifest mods table has no explicit INSERT/UPDATE policy for hosts. The `upsert_session_manifest` RPC handles it via `SECURITY DEFINER`, but direct PostgREST writes are blocked by RLS (which is correct). However, there's no SELECT policy — members can't read the manifest mods directly.

### 4.6 `essentials_sessions.join_token` was added then removed
Migration `0001_08` added `join_token` as a plaintext column, then `0002_08` removed it. The column still exists in the table schema (the `ALTER TABLE ADD COLUMN` ran, but no `DROP COLUMN` was issued). This means the plaintext token is still in the database.
**Recommendation:** Add `ALTER TABLE essentials_sessions DROP COLUMN IF EXISTS join_token;` to a new migration.

---

## 5. MEDIUM — Runtime Agent Production Readiness

### 5.1 No persistent config
The agent stores config in memory. If it crashes, it re-registers (which is fine), but there's no record of which servers it was managing.

### 5.2 No graceful shutdown for Java processes
The agent sends `stop` via stdin but doesn't wait for the process to actually exit before reporting completion. If the server hangs, the operation times out.

### 5.3 No operation timeout handling
The `server_operations.timeout_seconds` column exists but the agent never checks it. A stuck operation stays in `claimed`/`running` forever.

### 5.4 Console buffer is in-memory only
If the agent restarts, all console output since the last flush is lost. The `console_lines` table is the durable store, but there's a gap between flushes.

### 5.5 No backup rotation
The backup handler creates backups but never cleans up old ones. Disk usage grows unbounded.

### 5.6 No server jar auto-download
The agent requires a `server.jar` to already be placed manually. There's no integration with the Minecraft version manifest to auto-download the correct jar.

---

## 6. MEDIUM — Schema Inconsistencies

### 6.1 Two `servers` tables semantics
The `servers` table (migration 0005) is for dedicated servers with host/port. The `server_instances` table (runtime migration) is for managed servers on nodes. The launcher uses both but they're not clearly differentiated.

### 6.2 `essentials_sessions` vs `server_instances`
Essentials sessions are peer-to-peer WebRTC worlds. Server instances are dedicated hosted servers. The launcher has separate UIs for both, but the data model overlaps.

### 6.3 `hosting_nodes` vs `nodes`
We created `hosting_nodes` for the runtime agent, but the launcher uses `nodes`. These should be unified or clearly separated.

---

## 7. LOW — Missing Features

### 7.1 No Whop webhook handler
The `Whop live management` status is "waiting on provider permission." Once granted, a webhook Edge Function is needed to sync subscription changes in real-time.

### 7.2 No audit trail for node operations
When a runtime node starts/stops a server, there's no audit log. The `server_operations` table tracks individual ops but not a human-readable audit trail.

### 7.3 No backup storage integration
Backups are stored locally on the node's filesystem. There's no integration with cloud storage (S3, Supabase Storage) for off-node backup durability.

### 7.4 No monitoring/alerting
No integration with monitoring tools (Grafana, Prometheus, Datadog). Telemetry is written to Supabase but not surfaced in any dashboard.

### 7.5 No CI/CD for migrations
Migrations are applied manually. There's no GitHub Action or automated pipeline to apply migrations on deploy.

---

## Realistic Status by Feature

| Feature | Status | What's Missing |
|---|---|---|
| **Auth (signup/login/logout)** | ✅ Working | Nothing — Supabase Auth handles it |
| **Essentials Sessions** | ✅ Working | Nothing — fully migrated |
| **Essentials Signaling** | ✅ Working | Nothing — SDP/ICE via Supabase |
| **Essentials Addresses** | ✅ Working | Nothing — `.amalgam-essentials` namespace |
| **Server Addresses** | ✅ Working | Nothing — `.amalgam` namespace |
| **Project Publishing** | ✅ Working | Nothing — full moderation workflow |
| **Beta Feedback** | ✅ Working | Nothing — simple table + RPC |
| **Social Blocks** | ✅ Working | Nothing — block_user/unblock_user RPCs |
| **Admin User Directory** | ✅ Working | Nothing — staff-only RPC |
| **TURN Credentials** | ✅ Working | Nothing — Edge Function + Cloudflare |
| **Runtime Node Agent** | 🟡 Built, Unverified | Needs Supabase credentials to test live |
| **Java Edition Profiles** | ❌ Broken | `profiles` table missing |
| **Bedrock Profiles** | ❌ Broken | `bedrock_profiles` table missing |
| **Friend System** | ❌ Broken | 6 Edge Functions + 2 tables missing |
| **Messaging** | ❌ Broken | 5 Edge Functions + 3 tables missing |
| **Party System** | ❌ Broken | 6 Edge Functions + 2 tables missing |
| **User Presence** | ❌ Broken | 4 Edge Functions + 1 table missing |
| **User Search** | ❌ Broken | 1 Edge Function missing |
| **Subscriptions/Billing** | ❌ Broken | `subscriptions` table + Whop webhook missing |
| **TURN Usage Tracking** | ❌ Broken | `turn_usage` table missing |
| **Launcher Node Management** | ❌ Broken | `nodes` table missing (different from `hosting_nodes`) |
| **Whop Integration** | ⏳ Blocked | Waiting on provider permission |

---

## Priority Fix Order

1. **Create `profiles` table** — blocks profile sync for all users
2. **Create `subscriptions` table** — blocks Amalgam+ detection
3. **Create `nodes` table** (or rename `hosting_nodes`) — blocks server management
4. **Create `bedrock_profiles` table** — blocks Bedrock support
5. **Create `turn_usage` table** — blocks TURN quota display
6. **Create social Edge Functions + tables** — blocks all social features
7. **Fix `node_secret` to store hashes** — security hardening
8. **Add operation timeout handling** to runtime agent
9. **Add rate limiting** to high-traffic RPCs
10. **Add Whop webhook** when permission is granted
