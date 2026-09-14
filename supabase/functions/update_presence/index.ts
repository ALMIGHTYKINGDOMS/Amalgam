import { enforceRateLimit, getAuth, jsonResponse, readJson, sanitize, validateEnum, validateLength } from "../_shared/auth.ts";

const cors = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Headers": "authorization, x-client-info, apikey, content-type",
  "Content-Type": "application/json",
};

Deno.serve(async (request) => {
  if (request.method === "OPTIONS") return new Response("ok", { headers: cors });
  if (request.method !== "POST") return jsonResponse({ error: "method not allowed" }, 405, request);

  const { supabase, userId, error } = await getAuth(request);
  if (error) return jsonResponse({ error }, 401, request);

  try {
    const body = await readJson(request);
    const status = sanitize(body.status || "online", 32);
    const statusMessage = sanitize(body.status_message, 200);
    const currentServerId = sanitize(body.current_server_id || body.server_id, 128);
    const statusError = validateEnum(status, "status", ["online", "offline", "in_game", "away"], true);
    const messageError = validateLength(statusMessage, "status_message", 200);
    if (statusError || messageError) return jsonResponse({ error: statusError || messageError }, 400, request);

    const limited = await enforceRateLimit(supabase, "update_presence", 60);
    if (limited) return jsonResponse({ error: limited }, 429, request);

    const now = new Date().toISOString();
    const { error: presenceError } = await supabase.from("user_presence").upsert({
      user_id: userId,
      status,
      status_message: statusMessage,
      current_server_id: currentServerId,
      last_seen_at: now,
      updated_at: now,
    }, { onConflict: "user_id" });
    if (presenceError) return jsonResponse({ error: presenceError.message }, 400, request);

    const { error: profileError } = await supabase.from("public_profiles").upsert({
      user_id: userId,
      last_seen_at: now,
      updated_at: now,
    }, { onConflict: "user_id" });
    if (profileError) return jsonResponse({ error: profileError.message }, 400, request);

    return jsonResponse({ success: true, updated_at: now }, 200, request);
  } catch (err) {
    return jsonResponse({ error: err instanceof Error ? err.message : "invalid request" }, 400, request);
  }
});
