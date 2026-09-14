import { enforceRateLimit, getAuth, jsonResponse, readJson, sanitize, validateLength, validateUuid } from "../_shared/auth.ts";

Deno.serve(async (request) => {
  if (request.method === "OPTIONS") return new Response("ok", { headers: { "Access-Control-Allow-Origin": "*", "Access-Control-Allow-Headers": "authorization, x-client-info, apikey, content-type" } });
  if (request.method !== "POST") return jsonResponse({ error: "method not allowed" }, 405, request);
  const { supabase, error } = await getAuth(request);
  if (error) return jsonResponse({ error }, 401, request);
  try {
    const body = await readJson(request);
    const idError = validateUuid(body.receiver_id, "receiver_id");
    const message = sanitize(body.message, 500);
    const messageError = validateLength(message, "message", 500);
    if (idError || messageError) return jsonResponse({ error: idError || messageError }, 400, request);
    const limited = await enforceRateLimit(supabase, "send_friend_request", 20);
    if (limited) return jsonResponse({ error: limited }, 429, request);
    const { data, error: rpcError } = await supabase.rpc("send_friend_request", {
      p_receiver_id: body.receiver_id,
      p_message: message,
    });
    if (rpcError) return jsonResponse({ error: rpcError.message }, 400, request);
    return jsonResponse(data || { success: true }, 201, request);
  } catch (err) {
    return jsonResponse({ error: err instanceof Error ? err.message : "invalid request" }, 400, request);
  }
});
