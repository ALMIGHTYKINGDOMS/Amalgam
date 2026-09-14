# C++ Launcher + Runtime Agent — Cross-Reference Audit
## August 21, 2026

> **⚠️ SUPERSEDED** — This document was written before migrations
> `202608210003`–`202608210007` and the 21 social Edge Functions were added.
> Its central claims (6 missing tables, 21 missing Edge Functions) are no
> longer true: `profiles`, `bedrock_profiles`, `subscriptions`, `turn_usage`,
> `nodes`, the social tables, and all 21 functions now exist. See
> `docs/audit-2026-08-21-full.md` for the current consolidated audit. The
> per-line cross-reference tables below remain useful as a map, but the
> Status columns are outdated.

---

## 1. TABLE REFERENCE MAP

Every database table referenced by the C++ launcher or runtime agent, checked against actual Supabase migrations.

### C++ Launcher — Direct Table Access (PostgREST)

| Table | C++ File | Lines | Migration | Status |
|---|---|---|---|---|
| `servers` | supabase.cpp, sync_manager.cpp | 1516-1617 | 0005 | ✅ EXISTS (missing `current_players`, `last_ping` columns) |
| `profiles` | supabase.cpp | 1630-1791 | — | ❌ MISSING — no migration |
| `nodes` | supabase.cpp | 1867-2020 | — | ❌ MISSING — launcher uses `nodes`, we created `hosting_nodes` |
| `essentials_signals` | supabase.cpp | 2220-2255 | 0003 | ✅ EXISTS |
| `bedrock_profiles` | supabase.cpp | 2381-2600 | — | ❌ MISSING — no migration |
| `subscriptions` | supabase.cpp | 2270-2340 | — | ❌ MISSING — no migration |
| `turn_usage` | supabase.cpp | 2360+ | — | ❌ MISSING — no migration |
| `modpack_metadata` | storage_manager.cpp | 656-706 | 0001 | ✅ EXISTS |
| `projects` | project_publishing.cpp | 61-143 | 0001 | ✅ EXISTS |
| `project_versions` | project_publishing.cpp | 104 | 0001 | ✅ EXISTS |
| `project_media` | project_publishing.cpp | 134 | 0001 | ✅ EXISTS |
| `staff_roles` | (via is_project_staff) | — | 0001 | ✅ EXISTS |
| `blocked_users` | supabase.cpp | 2695-2708 | 0004_08 | ✅ EXISTS |
| `beta_feedback` | ui.cpp | 11414 | 0003_08 | ✅ EXISTS |

**Launcher result: 8 tables exist, 6 tables MISSING**

### Runtime Agent — Direct Table Access (PostgREST)

| Table | Agent File | Lines | Migration | Status |
|---|---|---|---|---|
| `server_operations` | operations.js | 25 | runtime_node | ✅ EXISTS |
| `server_instances` | operations.js | 96, 279 | runtime_node | ✅ EXISTS |

**Runtime agent result: 2 tables exist, 0 missing**

### Runtime Agent — RPC Calls

| RPC | Agent File | Migration | Status |
|---|---|---|---|
| `register_hosting_node` | heartbeat.js | runtime_node | ✅ EXISTS |
| `node_heartbeat` | heartbeat.js, index.js | runtime_node | ✅ EXISTS |
| `start_operation` | operations.js | runtime_node | ✅ EXISTS |
| `complete_operation` | operations.js | runtime_node | ✅ EXISTS |
| `update_server_instance` | operations.js | runtime_node | ✅ EXISTS |
| `write_console_lines` | console.js | runtime_node | ✅ EXISTS |
| `write_telemetry` | console.js | runtime_node | ✅ EXISTS |
| `insert_events` | events.js | data_collection | ✅ EXISTS |
| `insert_metrics` | events.js | data_collection | ✅ EXISTS |
| `insert_errors` | events.js | data_collection | ✅ EXISTS |
| `insert_feature_usage` | events.js | data_collection | ✅ EXISTS |

**Runtime agent RPC result: 11 RPCs exist, 0 missing**

---

## 2. EDGE FUNCTION MAP

Every Edge Function called by the C++ launcher, checked against `supabase/functions/`.

| Edge Function | C++ Method | Status | Required Backing Tables |
|---|---|---|---|
| `get-turn-credentials` | `request_turn_credentials()` | ✅ DEPLOYED | — |
| `get_friends` | `get_friends()` | ❌ MISSING | `friendships`, `public_profiles` |
| `get_friend_requests` | `get_friend_requests()` | ❌ MISSING | `friend_requests` |
| `send_friend_request` | `send_friend_request()` | ❌ MISSING | `friend_requests` |
| `accept_friend_request` | `accept_friend_request()` | ❌ MISSING | `friendships`, `friend_requests` |
| `reject_friend_request` | `reject_friend_request()` | ❌ MISSING | `friend_requests` |
| `remove_friend` | `remove_friend()` | ❌ MISSING | `friendships` |
| `search_users` | `search_users()` | ❌ MISSING | `public_profiles` |
| `get_conversations` | `get_conversations()` | ❌ MISSING | `conversations`, `conversation_participants` |
| `get_messages` | `get_messages()` | ❌ MISSING | `messages` |
| `send_message` | `send_message()` | ❌ MISSING | `messages`, `conversations` |
| `mark_messages_read` | `mark_messages_read()` | ❌ MISSING | `messages` |
| `get_or_create_conversation` | `get_or_create_conversation()` | ❌ MISSING | `conversations`, `conversation_participants` |
| `get_parties` | `get_parties()` | ❌ MISSING | `parties`, `party_members` |
| `create_party` | `create_party()` | ❌ MISSING | `parties`, `party_members` |
| `join_party` | `join_party()` | ❌ MISSING | `parties`, `party_members` |
| `leave_party` | `leave_party()` | ❌ MISSING | `party_members` |
| `disband_party` | `disband_party()` | ❌ MISSING | `parties`, `party_members` |
| `get_party_members` | `get_party_members()` | ❌ MISSING | `party_members` |
| `update_presence` | `update_presence()` | ❌ MISSING | `user_presence` |
| `get_user_presence` | `get_user_presence()` | ❌ MISSING | `user_presence` |
| `get_friends_presence` | `get_friends_presence()` | ❌ MISSING | `user_presence`, `friendships` |

**Edge Function result: 1 deployed, 21 MISSING**

---

## 3. RPC MAP (Launcher)

Every PostgREST RPC called by the C++ launcher.

| RPC | C++ File | Migration | Status |
|---|---|---|---|
| `is_project_staff` | supabase.cpp | 0001 | ✅ EXISTS |
| `block_user` | supabase.cpp | 0004_08 | ✅ EXISTS |
| `unblock_user` | supabase.cpp | 0004_08 | ✅ EXISTS |
| `get_blocked_users` | supabase.cpp | 0004_08 | ✅ EXISTS |
| `submit_beta_feedback` | ui.cpp | 0003_08 | ✅ EXISTS |
| `submit_project_for_review` | project_publishing.cpp | 0001 | ✅ EXISTS |
| `publish_project_version` | project_publishing.cpp | 0001 | ✅ EXISTS |
| `review_project` | project_publishing.cpp | 0001 | ✅ EXISTS |
| `create_session` | essentials_session.cpp | 0004 | ✅ EXISTS |
| `start_session` | essentials_session.cpp | 0004 | ✅ EXISTS |
| `stop_session` | essentials_session.cpp | 0004 | ✅ EXISTS |
| `update_session` | essentials_sync.cpp | 0004 | ✅ EXISTS |
| `join_session` | essentials_session.cpp | 0002 | ✅ EXISTS |
| `leave_session` | essentials_session.cpp | 0002 | ✅ EXISTS |
| `send_session_invite` | essentials_session.cpp | 0001_08 | ✅ EXISTS |
| `accept_session_invite` | essentials_session.cpp | 0001_08 | ✅ EXISTS |
| `decline_session_invite` | essentials_session.cpp | 0001_08 | ✅ EXISTS |
| `cancel_session_invite` | essentials_session.cpp | 0001_08 | ✅ EXISTS |
| `get_received_session_invites` | essentials_session.cpp | 0001_08 | ✅ EXISTS |
| `get_sent_session_invites` | essentials_session.cpp | 0001_08 | ✅ EXISTS |
| `kick_player` | essentials_session.cpp | 0001_08 | ✅ EXISTS |
| `ban_player` | essentials_session.cpp | 0001_08 | ✅ EXISTS |
| `unban_player` | essentials_session.cpp | 0001_08 | ✅ EXISTS |
| `upsert_session_manifest` | essentials_session.cpp | 0003 | ✅ EXISTS |
| `get_session_manifest` | essentials_session.cpp | 0002_08 | ✅ EXISTS |
| `is_essentials_alias_available` | essentials_address.cpp | 0004 | ✅ EXISTS |
| `is_server_alias_available` | essentials_address.cpp | 0005 | ✅ EXISTS |
| `resolve_essentials_address` | essentials_address.cpp | 0005 | ✅ EXISTS |
| `admin_list_users` | admin_ui.cpp | 0005_08 | ✅ EXISTS |

**Launcher RPC result: 29 RPCs exist, 0 missing** ✅

---

## 4. REALTIME SUBSCRIPTION MAP

| Subscription Topic | C++ Code | Required Table | Status |
|---|---|---|---|
| `servers` | supabase.cpp:2026 | `servers` | ✅ Table exists |
| `profiles` | supabase.cpp:2051 | `profiles` | ❌ Table MISSING |
| `bedrock_profiles` | supabase.cpp:2096 | `bedrock_profiles` | ❌ Table MISSING |
| `account_activity` | supabase.cpp:2157 | `account_activity` | ❌ Table MISSING |
| `messages` | supabase.cpp:2970 | `messages` | ❌ Table MISSING |
| `user_presence` | supabase.cpp:2987 | `user_presence` | ❌ Table MISSING |
| `friend_requests` | supabase.cpp:3003 | `friend_requests` | ❌ Table MISSING |
| `party_members` | supabase.cpp:3021 | `party_members` | ❌ Table MISSING |

**Realtime result: 1 table exists, 7 tables MISSING**

---

## 5. TABLE NAME CONFLICTS (Launcher vs Runtime)

| Launcher Uses | Runtime Uses | Conflict? | Resolution |
|---|---|---|---|
| `nodes` | `hosting_nodes` | ⚠️ YES | Launcher reads `nodes`, runtime writes `hosting_nodes`. Different tables. |
| `servers` | `server_instances` | ⚠️ PARTIAL | Launcher manages `servers` (dedicated). Runtime manages `server_instances` (managed). |
| `subscriptions` | — | No conflict | Launcher reads `subscriptions`, runtime doesn't touch it. |
| `turn_usage` | — | No conflict | Launcher reads `turn_usage`, runtime doesn't touch it. |

---

## 6. FEATURE COMPLETENESS MATRIX

| Feature | Launcher Code | DB Tables | Edge Functions | RPCs | Status |
|---|---|---|---|---|---|
| **Auth** | ✅ Built | ✅ Supabase Auth | — | — | ✅ WORKING |
| **Essentials Sessions** | ✅ Built | ✅ 6 tables | — | ✅ 12 RPCs | ✅ WORKING |
| **Essentials Signaling** | ✅ Built | ✅ signals | — | — | ✅ WORKING |
| **Essentials Addresses** | ✅ Built | ✅ alias cols | — | ✅ 3 RPCs | ✅ WORKING |
| **Server Addresses** | ✅ Built | ✅ servers | — | — | ✅ WORKING |
| **Project Publishing** | ✅ Built | ✅ 4 tables | — | ✅ 3 RPCs | ✅ WORKING |
| **Beta Feedback** | ✅ Built | ✅ table | — | ✅ 1 RPC | ✅ WORKING |
| **Social Blocks** | ✅ Built | ✅ table | — | ✅ 3 RPCs | ✅ WORKING |
| **Admin Directory** | ✅ Built | ✅ table | — | ✅ 1 RPC | ✅ WORKING |
| **TURN Credentials** | ✅ Built | — | ✅ deployed | — | ✅ WORKING |
| **Runtime Node Agent** | ✅ Built | ✅ 6 tables | — | ✅ 11 RPCs | 🟡 BUILT, UNTESTED LIVE |
| **Data Collection** | — | ✅ 5 tables | — | ✅ 5 RPCs | 🟡 BUILT, UNTESTED LIVE |
| **Java Profiles** | ✅ Built | ❌ MISSING | — | — | ❌ BROKEN |
| **Bedrock Profiles** | ✅ Built | ❌ MISSING | — | — | ❌ BROKEN |
| **Friend System** | ✅ Built | ❌ MISSING | ❌ 6 MISSING | — | ❌ BROKEN |
| **Messaging** | ✅ Built | ❌ MISSING | ❌ 5 MISSING | — | ❌ BROKEN |
| **Party System** | ✅ Built | ❌ MISSING | ❌ 6 MISSING | — | ❌ BROKEN |
| **User Presence** | ✅ Built | ❌ MISSING | ❌ 4 MISSING | — | ❌ BROKEN |
| **User Search** | ✅ Built | ❌ MISSING | ❌ 1 MISSING | — | ❌ BROKEN |
| **Subscriptions** | ✅ Built | ❌ MISSING | — | — | ❌ BROKEN |
| **TURN Usage** | ✅ Built | ❌ MISSING | — | — | ❌ BROKEN |
| **Launcher Nodes** | ✅ Built | ❌ MISSING | — | — | ❌ BROKEN |

---

## 7. PRODUCTION READINESS SCORES

| Component | Score | Notes |
|---|---|---|
| Supabase Auth | 10/10 | Fully managed by Supabase |
| Essentials Control Plane | 10/10 | All tables, RPCs, RLS working |
| Project Publishing | 10/10 | Full moderation workflow |
| TURN Credentials | 9/10 | Working, no usage tracking |
| C++ Launcher (Supabase) | 4/10 | 6 missing tables, 21 missing Edge Functions |
| Runtime Agent | 7/10 | Built and tested locally, needs live Supabase test |
| Data Collection | 6/10 | Schema + agent built, needs live test + launcher hooks |
| Social Features | 1/10 | Only RLS policies for blocks exist |
| Billing/Subscriptions | 0/10 | No tables, no webhook |
| Bedrock Support | 0/10 | No profile table |

---

## 8. PRIORITY FIX LIST

### Tier 1 — Tables the Launcher Directly Queries (crashes/silent failure)

1. **`profiles` table** — `get_profiles()`, `create_profile()` return empty
2. **`nodes` table** — `get_nodes()`, `register_node()` return empty
3. **`bedrock_profiles` table** — `get_bedrock_profiles()` returns empty
4. **`subscriptions` table** — `get_subscription()` returns empty
5. **`turn_usage` table** — `get_turn_usage()` returns empty
6. **`servers` missing columns** — `current_players`, `last_ping`, `last_started`, `assigned_at`

### Tier 2 — Social Edge Functions (21 missing)

7. Friend system: 6 Edge Functions + `friendships`, `friend_requests`, `public_profiles` tables
8. Messaging: 5 Edge Functions + `conversations`, `conversation_participants`, `messages` tables
9. Party system: 6 Edge Functions + `parties`, `party_members` tables
10. Presence: 4 Edge Functions + `user_presence` table

### Tier 3 — Security & Production

11. Hash `node_secret` (store SHA-256, not plaintext)
12. Add rate limiting to RPCs
13. Restrict `staff_roles` INSERT to existing staff
14. Redact emails from `admin_list_users`
15. Drop plaintext `join_token` column from `essentials_sessions`

### Tier 4 — Runtime Agent Hardening

16. Operation timeout handling
17. Backup rotation
18. Server jar auto-download from version manifest
19. Graceful process shutdown with wait
20. Systemd/PM2 service definition

### Tier 5 — Data Collection Integration

21. Add launcher-side event collection (C++ `EventCollector` class)
22. Add launcher error reporting to `errors` table
23. Add launcher feature usage tracking
24. Add Whop webhook Edge Function
