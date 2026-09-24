#!/usr/bin/env node
/**
 * Dependency-free Supabase source verifier.
 * This intentionally does not claim that the live project is deployed; it
 * checks only that the repository is internally consistent before deployment.
 */

import fs from "node:fs";
import path from "node:path";
import process from "node:process";

const root = process.cwd();
const migrationsDir = path.join(root, "supabase", "migrations");
const functionsDir = path.join(root, "supabase", "functions");
const configPath = path.join(root, "supabase", "config.toml");

const failures = [];
const warnings = [];
const check = (condition, message) => {
  if (!condition) failures.push(message);
};

const migrationFiles = fs.readdirSync(migrationsDir)
  .filter((name) => name.endsWith(".sql"))
  .sort();
check(migrationFiles.length > 0, "no SQL migrations found");
check(migrationFiles.some((name) => name.includes("202608210006")),
  "upgrade migration 202608210006_upgrade_and_polish.sql is missing");
check(migrationFiles.some((name) => name.includes("202608210007")),
  "production hardening migration 202608210007_production_hardening.sql is missing");
const rpcIdentityHardeningName = migrationFiles.find((name) => name.includes("202609220001"));
check(Boolean(rpcIdentityHardeningName),
  "RPC identity hardening migration 202609220001_rpc_identity_and_telemetry_hardening.sql is missing");

const migrationSources = [];
for (const name of migrationFiles) {
  const source = fs.readFileSync(path.join(migrationsDir, name), "utf8");
  migrationSources.push(source);
  const dollarCount = (source.match(/\$\$/g) || []).length;
  check(dollarCount % 2 === 0, `${name}: unmatched $$ function delimiter`);
  check(!/\bDROP\s+DATABASE\b/i.test(source), `${name}: destructive DROP DATABASE found`);
  check(!/\bTRUNCATE\b/i.test(source), `${name}: destructive TRUNCATE found`);
}
const allMigrations = migrationSources.join("\n");
const rpcIdentityHardening = rpcIdentityHardeningName
  ? fs.readFileSync(path.join(migrationsDir, rpcIdentityHardeningName), "utf8")
  : "";

// Replay the migrations statement by statement, in order, so a drop that
// precedes a create in the same file resolves the way the database would.
const statements = [];
for (const source of migrationSources) {
  // Comments are removed before splitting: these migrations document their
  // policies in prose that contains semicolons.
  for (const raw of source.replace(/--[^\n]*/g, " ").split(";")) {
    const statement = raw.replace(/\s+/g, " ").trim();
    if (statement) statements.push(statement);
  }
}

const createdTables = new Set();
const droppedTables = new Set();
const rlsTables = new Set();
const definedFunctions = new Set();

// RLS: a policy that amounts to "any signed-in user" is a policy the Edge
// Functions never needed -- they act as the caller while the work is done by
// SECURITY DEFINER RPCs -- and it exposes rows an RPC is supposed to own.
const policyKey = (table, name) => `${table.toLowerCase()}::${name.toLowerCase()}`;
const livePolicies = new Map();
for (const statement of statements) {
  let match;
  if ((match = /^create table (?:if not exists )?(?:public\.)?"?([a-z_]+)"?\s*\(/i.exec(statement)))
    createdTables.add(match[1].toLowerCase());
  if ((match = /^drop table (?:if exists )?(?:public\.)?"?([a-z_]+)"?/i.exec(statement)))
    droppedTables.add(match[1].toLowerCase());
  if ((match = /^alter table (?:if exists )?(?:only )?(?:public\.)?"?([a-z_]+)"?\s+enable row level security/i.exec(statement)))
    rlsTables.add(match[1].toLowerCase());
  if ((match = /^create (?:or replace )?function (?:public\.)?"?([a-z_]+)"?\s*\(/i.exec(statement)))
    definedFunctions.add(match[1].toLowerCase());
  if ((match = /^create policy\s+"?([^"]+?)"?\s+on\s+(?:public\.)?"?([a-z_]+)"?/i.exec(statement)))
    livePolicies.set(policyKey(match[2], match[1].trim()), statement);
  if ((match = /^drop policy(?: if exists)?\s+"?([^"]+?)"?\s+on\s+(?:public\.)?"?([a-z_]+)"?/i.exec(statement)))
    livePolicies.delete(policyKey(match[2], match[1].trim()));
}

const tables = [...createdTables].filter((table) => !droppedTables.has(table)).sort();
for (const table of tables) {
  check(rlsTables.has(table), `table "${table}" is created without enabling row level security`);
}

// plan_quotas is instance-wide plan configuration, not user data: every signed-in
// user reads the limits that apply to them and none of them can write. It is the
// only sanctioned blanket policy.
const publicConfigPolicies = new Set(["plan_quotas::anyone reads plan quotas"]);
for (const [key, statement] of livePolicies) {
  const blanket = /using\s*\(\s*true\s*\)/i.test(statement) || /with check\s*\(\s*true\s*\)/i.test(statement);
  if (!blanket) continue;
  check(publicConfigPolicies.has(key), `blanket RLS policy left in place: ${key}`);
}

// Telemetry and audit writes have intentionally scoped SECURITY DEFINER RPCs.
// Direct table insert policies would sidestep their caller identity and runtime
// node checks, so none of these permissive write paths may survive migration
// replay. Read policies are checked separately by the client/table contract.
for (const [table, policy] of [
  ["events", "users insert own events"],
  ["events", "runtime nodes insert events"],
  ["metrics", "launcher insert metrics"],
  ["errors", "users insert own errors"],
  ["errors", "runtime nodes insert errors"],
  ["feature_usage", "users insert own feature usage"],
  ["audit_log", "authenticated insert audit"],
  ["audit_log", "users insert own audit"],
]) {
  check(!livePolicies.has(policyKey(table, policy)),
    `direct telemetry/audit write policy remains after hardening: ${table}::${policy}`);
}

// These functions are reached from the public Data API, so lock their final
// bodies to a caller-safe search path and caller-derived identity. This is a
// source invariant only; a linked/local database policy test is still needed
// before claiming deployed-project verification.
for (const name of [
  "assign_server_to_node",
  "create_server_from_template",
  "insert_events",
  "insert_metrics",
  "insert_errors",
  "insert_feature_usage",
  "record_audit",
]) {
  check(rpcIdentityHardening.includes(`create or replace function public.${name}`),
    `RPC identity hardening does not redefine ${name}`);
}
const safeDefinerCount = (rpcIdentityHardening.match(/security definer set search_path = ''/g) || []).length;
check(safeDefinerCount >= 7,
  "RPC identity hardening must pin every redefined SECURITY DEFINER search_path");
for (const fragment of [
  "p_user_id is distinct from v_effective_user_id",
  "template not found or not available to user",
  "event user_id does not match authenticated caller",
  "metric user_id does not match authenticated caller",
  "error user_id does not match authenticated caller",
  "feature usage user_id does not match authenticated caller",
  "drop policy if exists \"users insert own audit\" on public.audit_log",
]) {
  check(rpcIdentityHardening.includes(fragment),
    `RPC identity hardening is missing invariant: ${fragment}`);
}

// Client/backend contract: the launcher must not call an Edge Function, table or
// RPC that this repository does not define, and every table it touches directly
// must be reachable -- RLS enabled and at least one policy granting access -- or
// PostgREST silently answers with an empty result set.
const clientDir = path.join(root, "cpp", "launcher", "src");
const clientSources = fs.readdirSync(clientDir)
  .filter((name) => name.endsWith(".cpp") || name.endsWith(".h"))
  .map((name) => fs.readFileSync(path.join(clientDir, name), "utf8"))
  .join("\n");
const calledFunctions = [...clientSources.matchAll(/call_edge_function\(\s*"([a-z0-9_-]+)"/g)]
  .map((match) => match[1]);
const functionNamesEarly = fs.readdirSync(functionsDir, { withFileTypes: true })
  .filter((entry) => entry.isDirectory() && !entry.name.startsWith("_"))
  .map((entry) => entry.name);
for (const name of new Set(calledFunctions)) {
  check(functionNamesEarly.includes(name), `client calls Edge Function "${name}" which is not in supabase/functions`);
}
for (const name of new Set([...clientSources.matchAll(/\.table\s*=\s*"([a-z_]+)"/g)].map((match) => match[1]))) {
  const table = name.toLowerCase();
  check(createdTables.has(table), `client reads table "${name}" which no migration creates`);
  if (createdTables.has(table) && !droppedTables.has(table)) {
    check(rlsTables.has(table), `client reads table "${name}" which never enables row level security`);
    check([...livePolicies.keys()].some((key) => key.startsWith(`${table}::`)),
      `client reads table "${name}" which has no policy, so every read returns empty`);
  }
}
for (const name of new Set([...clientSources.matchAll(/\.rpc\(\s*"([a-z_]+)"/g)].map((match) => match[1]))) {
  check(definedFunctions.has(name.toLowerCase()), `client calls RPC "${name}" which no migration defines`);
}

// The Edge Functions are the only callers of the SECURITY DEFINER RPCs, so an
// RPC they name must exist in a migration instead of failing at runtime.
for (const name of functionNamesEarly) {
  const entry = path.join(functionsDir, name, "index.ts");
  if (!fs.existsSync(entry)) continue;
  const source = fs.readFileSync(entry, "utf8");
  for (const match of source.matchAll(/\.rpc\(\s*"([a-z_]+)"/g)) {
    check(definedFunctions.has(match[1].toLowerCase()),
      `${name} calls RPC "${match[1]}" which no migration defines`);
  }
}

// A desktop launcher config must never declare a service-role field: even an
// empty compatibility placeholder normalizes a privileged credential into a
// client-side contract. validate-package.ps1 guards release output; this
// guards checked-in launcher JSON templates.
for (const name of fs.readdirSync(root).filter((entry) => /^launcher\.json/.test(entry))) {
  const source = fs.readFileSync(path.join(root, name), "utf8");
  check(!/"supabase_service_key"\s*:/.test(source),
    `${name} declares forbidden supabase_service_key; privileged Supabase keys belong only in server-side secret management`);
}

const functionNames = fs.readdirSync(functionsDir, { withFileTypes: true })
  .filter((entry) => entry.isDirectory() && !entry.name.startsWith("_"))
  .map((entry) => entry.name)
  .sort();
const config = fs.readFileSync(configPath, "utf8");
for (const name of functionNames) {
  const entry = path.join(functionsDir, name, "index.ts");
  check(fs.existsSync(entry), `${name}: index.ts is missing`);
  check(config.includes(`[functions.${name}]`), `${name}: missing config.toml function section`);
}

const sharedAuth = fs.readFileSync(path.join(functionsDir, "_shared", "auth.ts"), "utf8");
check(sharedAuth.includes("getAuth"), "shared auth helper does not export getAuth");
check(sharedAuth.includes("validateUuid"), "shared auth helper is missing UUID validation");

const webhook = fs.readFileSync(path.join(functionsDir, "whop-webhook", "index.ts"), "utf8");
check(webhook.includes("WHOP_WEBHOOK_SECRET"), "Whop webhook secret is not referenced");
check(/verify_jwt\s*=\s*false/.test(config), "Whop webhook must explicitly disable JWT verification");

const curseforgeProxy = fs.readFileSync(path.join(functionsDir, "curseforge-catalog", "index.ts"), "utf8");
check(curseforgeProxy.includes("CURSEFORGE_API_KEY"), "CurseForge proxy secret is not referenced");
check(curseforgeProxy.includes("allowedProviderUrl"), "CurseForge proxy is missing its path allowlist");
check(curseforgeProxy.includes('enforceRateLimit(supabase, "curseforge_catalog", 120)'),
  "CurseForge proxy must enforce an authenticated per-user rate limit");

const deployment = fs.readFileSync(path.join(root, "tools", "deploy-supabase.sh"), "utf8");
check(deployment.includes("set -euo pipefail"), "deployment script must fail fast with pipefail");
check(deployment.includes("supabase db push --linked"), "deployment script does not push linked migrations");
check(!deployment.includes("--no-verify-jwt"), "deployment script must not disable JWT verification globally");

if (failures.length) {
  console.error("Supabase source verification FAILED:");
  for (const failure of failures) console.error(`- ${failure}`);
  process.exit(1);
}

if (warnings.length) {
  for (const warning of warnings) console.warn(`WARNING: ${warning}`);
}

console.log(`Supabase source verification passed: ${migrationFiles.length} migrations, ${functionNames.length} Edge Functions.`);
console.log("This is a local source check; it does not verify the live Supabase project.");
