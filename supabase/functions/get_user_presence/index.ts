import { getAuth, jsonResponse } from "../_shared/auth.ts";

Deno.serve(async (request) => {
  if (request.method === "OPTIONS") return new Response("ok", { headers: { "Access-Control-Allow-Origin": "*", "Access-Control-Allow-Headers": "authorization, x-client-info, apikey, content-type" } });
  if (request.method !== "POST") return jsonResponse({ error: "method not allowed" }, 405);

  const { supabase, userId, error } = await getAuth(request);
  if (error) return jsonResponse({ error }, 401);
  const { data: limited } = await supabase.rpc("enforce_rate_limit", { p_action: "get_user_presence", p_max_per_minute: 60 });
  if (limited === false) return jsonResponse({ error: "rate limit exceeded" }, 429, request);

  const body = await request.json();
  const targetUserId = body.user_id;
  if (!targetUserId) return jsonResponse({ error: "user_id required" }, 400);

  const { data: presence } = await supabase
    .from("user_presence")
    .select("*")
    .eq("user_id", targetUserId)
    .single();

  if (!presence) {
    return jsonResponse({
      user_id: targetUserId,
      status: "offline",
      status_message: "",
      current_server_id: "",
      last_seen_at: 0,
      updated_at: 0,
    });
  }

  return jsonResponse({
    user_id: presence.user_id,
    status: presence.status,
    status_message: presence.status_message,
    current_server_id: presence.current_server_id,
    last_seen_at: Math.floor(new Date(presence.last_seen_at).getTime() / 1000),
    updated_at: Math.floor(new Date(presence.updated_at).getTime() / 1000),
  });
});
