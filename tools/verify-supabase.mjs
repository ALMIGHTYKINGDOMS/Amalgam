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

for (const name of migrationFiles) {
  const source = fs.readFileSync(path.join(migrationsDir, name), "utf8");
  const dollarCount = (source.match(/\$\$/g) || []).length;
  check(dollarCount % 2 === 0, `${name}: unmatched $$ function delimiter`);
  check(!/\bDROP\s+DATABASE\b/i.test(source), `${name}: destructive DROP DATABASE found`);
  check(!/\bTRUNCATE\b/i.test(source), `${name}: destructive TRUNCATE found`);
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
