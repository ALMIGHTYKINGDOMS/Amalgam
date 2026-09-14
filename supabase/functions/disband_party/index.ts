import { getAuth, jsonResponse, readJson, validateUuid } from "../_shared/auth.ts";

Deno.serve(async (request) => {
  if (request.method === "OPTIONS") return new Response("ok", { headers: { "Access-Control-Allow-Origin": "*", "Access-Control-Allow-Headers": "authorization, x-client-info, apikey, content-type" } });
  if (request.method !== "POST") return jsonResponse({ error: "method not allowed" }, 405, request);
  const { supabase, error } = await getAuth(request);
  if (error) return jsonResponse({ error }, 401, request);
  const { data: limited } = await supabase.rpc("enforce_rate_limit", { p_action: "disband_party", p_max_per_minute: 10 });
  if (limited === false) return jsonResponse({ error: "rate limit exceeded" }, 429, request);
  try {
    const body = await readJson(request);
    const validation = validateUuid(body.party_id, "party_id");
    if (validation) return jsonResponse({ error: validation }, 400, request);
    const { data, error: rpcError } = await supabase.rpc("disband_party", { p_party_id: body.party_id });
    if (rpcError) return jsonResponse({ error: rpcError.message }, 400, request);
    return jsonResponse(data || { success: true }, 200, request);
  } catch (err) {
    return jsonResponse({ error: err instanceof Error ? err.message : "invalid request" }, 400, request);
  }
});
