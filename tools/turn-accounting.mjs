// TURN accounting reference model.
//
// Mirrors the production PL/pgSQL in
// supabase/migrations/202608210008_rate_limit_privacy_turn.sql:
//   - ingest_turn_usage  (service_role only, replay-protected, atomic add)
//   - rollover_turn_period
//   - enforce_turn_quota (backend-owned allowance, TURNQUOTA exhaustion)
//
// This harness is TEST-ONLY. It never weakens production authentication:
// the role check below is the same gate the SQL enforces, and callers of
// the harness must still pass 'service_role' for an ingest to land.
//
// Fixtures (plan quotas, subscriptions) are supplied by the caller so this
// model can later be driven by real provider data without code changes.

export const FREE_ALLOWANCE_BYTES = 1073741824n; // 1 GiB, matches plan_quotas seed
export const PLUS_ALLOWANCE_BYTES = 85899345920n; // 80 GiB, matches plan_quotas seed
export const DEFAULT_ALLOWANCE_BYTES = 1073741824n; // SQL final fallback
export const PERIOD_SECONDS = 2592000n; // 30 days, matches rollover SQL

export class TurnAccountingError extends Error {
  constructor(message, code, detail) {
    super(message);
    this.code = code;
    this.detail = detail;
  }
}

export class TurnAccounting {
  // plans: Map plan -> allowanceBytes (bigint)
  // subscriptions: Map userId -> { plan, status }
  // now(): epoch seconds (injectable clock for rollover tests)
  constructor({ plans = new Map(), subscriptions = new Map(), now = () => Math.floor(Date.now() / 1000) } = {}) {
    this.plans = plans;
    this.subscriptions = subscriptions;
    this.now = now;
    this.usage = new Map(); // userId -> { used_bytes, monthly_bytes, period_start, period_end, last_source }
    this.eventIds = new Set(); // append-only ledger event ids
    this.events = []; // event log for auditing
  }

  _assertServiceRole(role) {
    if (role !== "service_role") {
      throw new TurnAccountingError("not authorized", "AUTH");
    }
  }

  // Mirrors ingest_turn_usage(p_user_id, p_bytes, p_source, p_event_id).
  ingest(userId, bytes, source = "relay", eventId = null, role = "service_role") {
    this._assertServiceRole(role);
    if (typeof bytes !== "bigint" || bytes < 0n) {
      throw new TurnAccountingError("bytes must be non-negative", "BYTES");
    }
    if (userId == null || !this.subscriptions.has(userId)) {
      // SQL checks auth.users membership; the harness uses the subscription
      // registry as its user table.
      throw new TurnAccountingError("user not found", "NOUSER");
    }

    if (eventId != null && this.eventIds.has(eventId)) {
      return { ingested: false, duplicate: true };
    }

    if (eventId != null) this.eventIds.add(eventId);
    this.events.push({ userId, bytes, source, eventId, ts: this.now() });

    this._rollover(userId);

    const row = this.usage.get(userId);
    if (row) {
      row.used_bytes += bytes;
      row.last_source = source;
    } else {
      const t = this.now();
      this.usage.set(userId, {
        used_bytes: bytes,
        monthly_bytes: 0n,
        period_start: BigInt(t),
        period_end: BigInt(t) + PERIOD_SECONDS,
        last_source: source,
      });
    }
    return { ingested: true };
  }

  // Mirrors rollover_turn_period.
  _rollover(userId) {
    const row = this.usage.get(userId);
    if (!row || row.period_end <= 0n) return;
    if (row.period_end < BigInt(this.now())) {
      row.used_bytes = 0n;
      row.period_start = row.period_end;
      row.period_end += PERIOD_SECONDS;
    }
  }

  // Mirrors enforce_turn_quota(); throws TURNQUOTA when exhausted.
  enforceQuota(userId) {
    this._rollover(userId);

    let allowance = 0n;
    const sub = this.subscriptions.get(userId);
    if (sub && sub.status === "active") {
      allowance = this.plans.get(String(sub.plan).toLowerCase()) ?? 0n;
    }
    if (allowance === 0n) allowance = this.plans.get("free") ?? 0n;
    if (allowance === 0n) allowance = this.usage.get(userId)?.monthly_bytes ?? 0n;
    if (allowance === 0n) allowance = DEFAULT_ALLOWANCE_BYTES;

    let used = 0n;
    let periodEnd = 0n;
    const row = this.usage.get(userId);
    if (row) {
      used = row.used_bytes;
      periodEnd = row.period_end;
    }
    if (periodEnd === 0n) {
      const t = this.now();
      this.usage.set(userId, {
        used_bytes: 0n,
        monthly_bytes: allowance,
        period_start: BigInt(t),
        period_end: BigInt(t) + PERIOD_SECONDS,
        last_source: "quota-init",
      });
      periodEnd = BigInt(t) + PERIOD_SECONDS;
    }

    if (used >= allowance) {
      throw new TurnAccountingError(
        "TURN relay allowance exhausted",
        "TURNQUOTA",
        { used_bytes: used, allowance, period_end: periodEnd },
      );
    }
    return {
      allowed: true,
      used_bytes: used,
      allowance,
      remaining_bytes: allowance - used,
      period_end: periodEnd,
    };
  }

  // Warning thresholds used by the launcher UI (75% / 90% / 100%).
  status(userId) {
    let q;
    try {
      q = this.enforceQuota(userId);
    } catch (e) {
      if (e.code === "TURNQUOTA") {
        return {
          used_bytes: e.detail.used_bytes,
          allowance: e.detail.allowance,
          remaining_bytes: 0n,
          pct: 100,
          threshold: "exhausted",
          allowed: false,
        };
      }
      throw e;
    }
    const pct = q.allowance === 0n ? 100 : Number((q.used_bytes * 10000n) / q.allowance) / 100;
    const threshold = pct >= 100 ? "exhausted" : pct >= 90 ? "warning-90" : pct >= 75 ? "warning-75" : "ok";
    return { ...q, pct, threshold, allowed: true };
  }
}

// Helpers shared with tests.
export function gb(n) {
  return BigInt(n) * 1000000000n; // decimal GB as the product docs state
}

export function gib(n) {
  return BigInt(n) * 1073741824n; // binary GiB as the SQL seed states
}
