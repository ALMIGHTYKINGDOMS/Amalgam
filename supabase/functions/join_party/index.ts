import { enforceRateLimit, getAuth, jsonResponse, readJson, sanitize, validateLength } from "../_shared/auth.ts";

Deno.serve(async (request) => {
  if (request.method === "OPTIONS") return new Response("ok", { headers: { "Access-Control-Allow-Origin": "*", "Access-Control-Allow-Headers": "authorization, x-client-info, apikey, content-type" } });
  if (request.method !== "POST") return jsonResponse({ error: "method not allowed" }, 405, request);
  const { supabase, error } = await getAuth(request);
  if (error) return jsonResponse({ error }, 401, request);
  try {
    const body = await readJson(request);
    const inviteCode = sanitize(body.invite_code, 8).toUpperCase();
    const validation = validateLength(inviteCode, "invite_code", 8, true);
    if (validation || inviteCode.length !== 8) return jsonResponse({ error: "invite_code must be 8 characters" }, 400, request);
    const limited = await enforceRateLimit(supabase, "join_party", 30);
    if (limited) return jsonResponse({ error: limited }, 429, request);
    const { data, error: rpcError } = await supabase.rpc("join_party", { p_invite_code: inviteCode });
    if (rpcError) return jsonResponse({ error: rpcError.message }, 400, request);
    return jsonResponse(data, 200, request);
  } catch (err) {
    return jsonResponse({ error: err instanceof Error ? err.message : "invalid request" }, 400, request);
  }
});
