#!/usr/bin/env node
// Rate-limit policy matrix (reference model).
//
// The authoritative enforcement is the SQL RPC `enforce_rate_limit`
// (rate_limits table, per-user per-action 60s fixed window) plus the
// `enforceRateLimit` calls in each Edge Function. This module extracts the
// REAL per-function limits from source and provides a faithful reference
// model so the policy shape (normal use, bursts, independent users, window
// reset, polling safety) can be proven without a live database.

import fs from "node:fs";
import path from "node:path";

export const WINDOW_SECONDS = 60;

/** Extract { action -> maxPerMinute } from every edge function's source. */
export function extractLimits(
  functionsDir = path.resolve(import.meta.dirname, "..", "supabase", "functions")
) {
  const limits = new Map();
  const noLimit = [];
  const dirs = fs
    .readdirSync(functionsDir, { withFileTypes: true })
    .filter((d) => d.isDirectory());
  for (const d of dirs) {
    const file = path.join(functionsDir, d.name, "index.ts");
    if (!fs.existsSync(file)) continue;
    const src = fs.readFileSync(file, "utf-8");
    // Forms: enforce_rate_limit, { p_action: "...", p_max_per_minute: N }
    //   or enforceRateLimit(supabase, "action", N)
    const rpcMatches = [...src.matchAll(/p_action:\s*"([a-z_]+)"[\s\S]*?p_max_per_minute:\s*(\d+)/g)];
    const fnMatches = [...src.matchAll(/enforceRateLimit\(\s*supabase,\s*"([a-z_]+)"\s*,\s*(\d+)\s*\)/g)];
    const found = [...rpcMatches, ...fnMatches];
    if (found.length === 0) {
      noLimit.push(d.name);
      continue;
    }
    for (const m of found) {
      limits.set(m[1], Number(m[2]));
    }
  }
  return { limits, noLimit };
}

/**
 * Fixed-window per-(user, action) limiter mirroring enforce_rate_limit:
 * count events in the current 60s window; once count >= max, reject until
 * the window rolls over.
 */
export class RateLimitModel {
  constructor(limits, { now = () => Date.now() / 1000 } = {}) {
    this.limits = limits; // Map action -> max per window
    this.now = now;
    this.state = new Map(); // `${user}|${action}` -> { windowStart, count }
  }

  /** Returns true when the request is allowed, false when rate-limited. */
  check(user, action) {
    const max = this.limits.get(action);
    if (max === undefined || max === null) return true; // no limit configured
    const t = this.now();
    const key = `${user}|${action}`;
    let s = this.state.get(key);
    if (!s || t - s.windowStart >= WINDOW_SECONDS) {
      s = { windowStart: t, count: 0 };
      this.state.set(key, s);
    }
    if (s.count >= max) return false;
    s.count += 1;
    return true;
  }
}

// Representative buckets verified against extracted source:
export const BUCKETS = {
  READS: 60,       // get_friends, presence, messages, conversations
  WRITES: 20,      // friend requests, party ops, messages, invites
  EXPENSIVE: 10,   // get_turn_credentials, session/join-code paths
};

export function classify(action) {
  // Expensive/abuse-sensitive actions first: get_turn_credentials is a read
  // in shape but must not share the generous read bucket.
  if (action.includes("turn") || action.includes("session") || action.includes("join")) {
    return "EXPENSIVE";
  }
  if (action.includes("get_") || action.includes("list") || action.includes("presence")) {
    return "READS";
  }
  return "WRITES";
}
