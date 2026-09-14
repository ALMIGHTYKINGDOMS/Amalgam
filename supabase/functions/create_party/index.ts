import { enforceRateLimit, getAuth, jsonResponse, readJson, sanitize, validateLength, validateRange } from "../_shared/auth.ts";

Deno.serve(async (request) => {
  if (request.method === "OPTIONS") return new Response("ok", { headers: { "Access-Control-Allow-Origin": "*", "Access-Control-Allow-Headers": "authorization, x-client-info, apikey, content-type" } });
  if (request.method !== "POST") return jsonResponse({ error: "method not allowed" }, 405, request);
  const { supabase, error } = await getAuth(request);
  if (error) return jsonResponse({ error }, 401, request);
  try {
    const body = await readJson(request);
    const name = sanitize(body.name || "Party", 64);
    const maxMembers = Math.min(Math.max(Number(body.max_members || 10), 2), 50);
    const nameError = validateLength(name, "name", 64, true);
    const rangeError = validateRange(maxMembers, "max_members", 2, 50, true);
    if (nameError || rangeError) return jsonResponse({ error: nameError || rangeError }, 400, request);
    const limited = await enforceRateLimit(supabase, "create_party", 10);
    if (limited) return jsonResponse({ error: limited }, 429, request);
    const { data, error: rpcError } = await supabase.rpc("create_party", {
      p_name: name,
      p_is_public: body.is_public !== false,
      p_max_members: maxMembers,
    });
    if (rpcError) return jsonResponse({ error: rpcError.message }, 400, request);
    return jsonResponse(data, 201, request);
  } catch (err) {
    return jsonResponse({ error: err instanceof Error ? err.message : "invalid request" }, 400, request);
  }
});
