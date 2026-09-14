import { createClient } from "https://esm.sh/@supabase/supabase-js@2";

const cors = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Headers": "authorization, x-client-info, apikey, content-type",
  "Content-Type": "application/json",
};

function json(body: unknown, status = 200) {
  return new Response(JSON.stringify(body), { status, headers: cors });
}

Deno.serve(async (request) => {
  if (request.method === "OPTIONS") return new Response("ok", { headers: cors });
  if (request.method !== "POST") return json({ error: "method not allowed" }, 405);

  const authorization = request.headers.get("Authorization");
  const supabaseUrl = Deno.env.get("SUPABASE_URL");
  const supabaseAnonKey = Deno.env.get("SUPABASE_ANON_KEY");
  const turnTokenId = Deno.env.get("CF_TURN_TOKEN_ID");
  const turnApiToken = Deno.env.get("CF_TURN_API_TOKEN");

  if (!authorization || !supabaseUrl || !supabaseAnonKey) {
    return json({ error: "authentication is required" }, 401);
  }
  if (!turnTokenId || !turnApiToken) {
    return json({ error: "TURN service is not configured" }, 503);
  }

  const supabase = createClient(supabaseUrl, supabaseAnonKey, {
    global: { headers: { Authorization: authorization } },
  });
  const { data: userData, error: userError } = await supabase.auth.getUser();
  if (userError || !userData.user) return json({ error: "invalid session" }, 401);

  // Credential issuance is expensive and spam-prone: bound it per user.
  const { data: limited } = await supabase.rpc("enforce_rate_limit", {
    p_action: "get_turn_credentials",
    p_max_per_minute: 10,
  });
  if (limited === false) return json({ error: "rate limit exceeded" }, 429);

  // Server-authoritative relay allowance check. Only relay allocation is
  // gated; direct peer-to-peer never needs these credentials and is
  // intentionally unaffected when the allowance is exhausted.
  const { data: quota, error: quotaError } = await supabase.rpc("enforce_turn_quota");
  if (quotaError) {
    if (quotaError.code === "TURNQUOTA") {
      return json(
        {
          error: "relay allowance exhausted; direct peer-to-peer still available",
          code: "TURN_QUOTA_EXHAUSTED",
        },
        429,
      );
    }
    return json({ error: "relay quota check failed" }, 502);
  }
  const remainingBytes = typeof quota?.remaining_bytes === "number" ? quota.remaining_bytes : null;

  let ttl = 3600;
  try {
    const body = await request.json();
    if (Number.isFinite(body?.ttl)) ttl = Math.max(60, Math.min(3600, Math.floor(body.ttl)));
  } catch {
    // The default TTL is valid when the request body is empty.
  }

  const endpoint = `https://rtc.live.cloudflare.com/v1/turn/keys/${encodeURIComponent(turnTokenId)}/credentials/generate-ice-servers`;
  const response = await fetch(endpoint, {
    method: "POST",
    headers: {
      "Authorization": `Bearer ${turnApiToken}`,
      "Content-Type": "application/json",
    },
    body: JSON.stringify({ ttl }),
  });
  const text = await response.text();
  if (!response.ok) return json({ error: "Cloudflare TURN credential request failed", detail: text }, 502);

  try {
    const parsed = JSON.parse(text);
    if (remainingBytes !== null) parsed.remaining_bytes = remainingBytes;
    return json({ user_id: userData.user.id, ttl, ...parsed });
  } catch {
    return json({ error: "Cloudflare returned invalid TURN credentials" }, 502);
  }
});
