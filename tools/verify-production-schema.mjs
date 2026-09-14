#!/usr/bin/env node
// Static production-schema verifier.
//
// Inspects supabase/migrations and supabase/functions WITHOUT a live database
// and checks the FINAL migration state for security invariants that a partial
// or reordered deployment could silently violate. Prints a PASS/FAIL report
// and exits non-zero on any failure.
//
// Usage: node tools/verify-production-schema.mjs

import { readFileSync, readdirSync, existsSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const migrationsDir = join(root, "supabase", "migrations");
const functionsDir = join(root, "supabase", "functions");

const results = [];
let failures = 0;

function check(name, ok, detail = "") {
  results.push({ name, ok, detail });
  if (!ok) failures += 1;
}

function migrations() {
  return readdirSync(migrationsDir).filter((f) => f.endsWith(".sql")).sort();
}

function readAll() {
  const out = {};
  for (const f of migrations()) out[f] = readFileSync(join(migrationsDir, f), "utf8");
  return out;
}

function lastMention(content, needle) {
  let idx = -1;
  let at = 0;
  while ((at = content.indexOf(needle, at)) !== -1) {
    idx = at;
    at += needle.length;
  }
  return idx;
}

// --- 1. Migration set -------------------------------------------------------
const files = migrations();
const required = [
  "202608170001_project_publishing.sql",
  "202608170002_essentials_control_plane.sql",
  "202608170003_essentials_signaling.sql",
  "202608170004_essentials_addresses.sql",
  "202608170005_server_addresses.sql",
  "202608180001_essentials_rpc_functions.sql",
  "202608180002_remove_plaintext_join_tokens.sql",
  "202608180003_beta_feedback.sql",
  "202608180004_social_blocks.sql",
  "202608180005_admin_user_directory.sql",
  "202608210001_runtime_node.sql",
  "202608210002_data_collection.sql",
  "202608210003_launcher_tables.sql",
  "202608210004_social_tables.sql",
  "202608210005_security_hardening.sql",
  "202608210006_upgrade_and_polish.sql",
  "202608210007_production_hardening.sql",
  "202608210008_rate_limit_privacy_turn.sql",
];
for (const r of required) {
  check(`migration present: ${r}`, files.includes(r));
}
check("migration count is current", files.length >= 21,
  `found ${files.length}`);

const all = readAll();
const joined = files.map((f) => all[f]).join("\n");
const joinedOrdered = files.map((f) => `${f}\n${all[f]}`).join("\n");

// --- 2. Dangerous policies must not survive to the final state ---------------
const dangerousPolicies = [
  "backend manages subscriptions",
  "system manages friendships",
  "authenticated create conversations",
  "authenticated add conversation participants",
  "authenticated create parties",
  "owners manage parties",
  "system insert activity",
  "authenticated insert audit",
];
for (const policy of dangerousPolicies) {
  const createNeedle = `create policy "${policy}"`;
  const dropNeedle = `drop policy if exists "${policy}"`;
  const hasCreate = joined.includes(createNeedle);
  const lastCreate = lastMention(joinedOrdered, createNeedle);
  const lastDrop = lastMention(joinedOrdered, dropNeedle);
  // Final state safe when: never created, or last occurrence is a drop AFTER the last create.
  const safe = !hasCreate || (lastDrop !== -1 && lastDrop > lastCreate);
  check(`dangerous policy removed: "${policy}"`, safe,
    safe ? "" : `created at char ${lastCreate}, drop at ${lastDrop}`);
}

// --- 3. RLS enabled on key tables --------------------------------------------
const rlsTables = [
  "profiles", "bedrock_profiles", "subscriptions", "turn_usage", "nodes",
  "friendships", "friend_requests", "public_profiles", "conversations",
  "conversation_participants", "messages", "parties", "party_members",
  "user_presence", "account_activity", "hosting_nodes", "server_instances",
  "server_operations", "blocked_users", "beta_feedback", "plan_quotas",
  "turn_usage_events",
];
for (const t of rlsTables) {
  const ok = new RegExp(
    `alter table (if not exists )?public\\.${t} enable row level security`,
    "i",
  ).test(joined);
  check(`RLS enabled: ${t}`, ok);
}

// --- 4. No authenticated write paths on quota/billing tables -----------------
function survivingAuthenticatedWritePolicy(table) {
  // A write policy targeting authenticated SURVIVES only if its last mention
  // in migration order is a create that is never later dropped. Dropped
  // policies (e.g. 007 removing "backend manages subscriptions") are safe.
  const re = new RegExp(
    `create policy "([^"]+)"\\s+on public\\.${table} for (insert|update|delete|all) to authenticated`,
    "gi",
  );
  let match;
  while ((match = re.exec(joinedOrdered)) !== null) {
    const name = match[1];
    const createNeedle = `create policy "${name}"`;
    const dropNeedle = `drop policy if exists "${name}"`;
    const lastCreate = lastMention(joinedOrdered, createNeedle);
    const lastDrop = lastMention(joinedOrdered, dropNeedle);
    if (lastDrop === -1 || lastDrop < lastCreate) return name;
  }
  return null;
}

for (const t of ["turn_usage", "subscriptions", "turn_usage_events"]) {
  const survivor = survivingAuthenticatedWritePolicy(t);
  check(`no authenticated write policy: ${t}`, survivor === null,
    survivor ? `policy "${survivor}" survives to final state` : "");
}

// --- 5. Trusted ingestion is service-role only -------------------------------
const ingestFn = "create or replace function public.ingest_turn_usage";
const ingestRejectsUsers = joined.includes("caller_role <> 'service_role'");
const ingestGrantedAuth = /grant execute on function public\.ingest_turn_usage[^;]*to authenticated/.test(joined);
const ingestGrantedSvc = /grant execute on function public\.ingest_turn_usage[^;]*to service_role/.test(joined);
check("ingest_turn_usage exists", joined.includes(ingestFn));
check("ingest_turn_usage rejects non-service_role callers", ingestRejectsUsers);
check("ingest_turn_usage NOT granted to authenticated", !ingestGrantedAuth);
check("ingest_turn_usage granted to service_role", ingestGrantedSvc);
check("turn_usage_events ledger has an event_id uniqueness guard",
  /create unique index[^;]*turn_usage_events_event_uidx/.test(joined));

// --- 6. Plaintext node_secret dropped in final state --------------------------
const nodeSecretDrop = /alter table public\.hosting_nodes drop column if exists node_secret/.test(joined);
check("plaintext node_secret column dropped", nodeSecretDrop);

// --- 7. Rate limiting is active on write paths --------------------------------
// Edge functions must call enforceRateLimit, and/or table triggers must exist.
const triggerRe = /create trigger trg_rate_limit_[a-z_]+/g;
const triggerCount = (joined.match(triggerRe) || []).length;
check("rate-limit table triggers exist", triggerCount >= 5, `${triggerCount} triggers`);

if (existsSync(functionsDir)) {
  let edgeLimited = 0;
  let edgeTotal = 0;
  for (const dir of readdirSync(functionsDir, { withFileTypes: true })) {
    if (!dir.isDirectory() || dir.name === "_shared") continue;
    const idx = join(functionsDir, dir.name, "index.ts");
    if (!existsSync(idx)) continue;
    edgeTotal += 1;
    const src = readFileSync(idx, "utf8");
    if (src.includes("enforceRateLimit(") || src.includes('rpc("enforce_rate_limit"')) edgeLimited += 1;
  }
  check("edge functions enforce rate limits", edgeLimited >= Math.min(6, edgeTotal),
    `${edgeLimited}/${edgeTotal} call enforceRateLimit`);
}

// --- 8. TURN quota enforcement wired ------------------------------------------
check("enforce_turn_quota defined", joined.includes("create or replace function public.enforce_turn_quota"));
const credsFn = join(functionsDir, "get-turn-credentials", "index.ts");
if (existsSync(credsFn)) {
  const creds = readFileSync(credsFn, "utf8");
  check("get-turn-credentials enforces quota", creds.includes('rpc("enforce_turn_quota")'));
  check("get-turn-credentials 429 on exhaustion", creds.includes("TURN_QUOTA_EXHAUSTED"));
}

// --- 9. Admin privacy ----------------------------------------------------------
check("admin_list_users masks emails", /'email',\s*case\s+when\s+u\.email is null/.test(joined) ||
  joined.includes("'***@'"));
check("admin_get_user_detail staff-gated", joined.includes("admin_get_user_detail") &&
  joined.includes("public.is_project_staff()"));
check("shared auth profile bootstrap exists",
  joined.includes("bootstrap_shared_user_profile") &&
  joined.includes("on_auth_user_created_bootstrap_profile") &&
  joined.includes("after insert on auth.users"));

// ------------------------------------------------------------------------------
console.log("AMALGAM PRODUCTION SCHEMA VERIFIER");
console.log("===================================");
for (const r of results) {
  console.log(`${r.ok ? "PASS" : "FAIL"}  ${r.name}${r.detail ? "  (" + r.detail + ")" : ""}`);
}
console.log("===================================");
console.log(`${results.length - failures}/${results.length} checks passed`);
process.exit(failures === 0 ? 0 : 1);
