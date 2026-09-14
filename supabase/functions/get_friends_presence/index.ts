import { getAuth, jsonResponse } from "../_shared/auth.ts";

Deno.serve(async (request) => {
  if (request.method === "OPTIONS") return new Response("ok", { headers: { "Access-Control-Allow-Origin": "*", "Access-Control-Allow-Headers": "authorization, x-client-info, apikey, content-type" } });
  if (request.method !== "POST") return jsonResponse({ error: "method not allowed" }, 405);

  const { supabase, userId, error } = await getAuth(request);
  if (error) return jsonResponse({ error }, 401);
  const { data: limited } = await supabase.rpc("enforce_rate_limit", { p_action: "get_friends_presence", p_max_per_minute: 60 });
  if (limited === false) return jsonResponse({ error: "rate limit exceeded" }, 429, request);

  // Get friend IDs
  const { data: friendships } = await supabase
    .from("friendships")
    .select("user_id_a, user_id_b")
    .eq("status", "accepted")
    .or(`user_id_a.eq.${userId},user_id_b.eq.${userId}`);

  if (!friendships || friendships.length === 0) return jsonResponse([]);

  const friendIds = friendships.map((f: any) =>
    f.user_id_a === userId ? f.user_id_b : f.user_id_a
  );

  // Get presence for all friends
  const { data: presences } = await supabase
    .from("user_presence")
    .select("*")
    .in("user_id", friendIds);

  const result = (presences || []).map((p: any) => ({
    user_id: p.user_id,
    status: p.status,
    status_message: p.status_message,
    current_server_id: p.current_server_id,
    last_seen_at: Math.floor(new Date(p.last_seen_at).getTime() / 1000),
    updated_at: Math.floor(new Date(p.updated_at).getTime() / 1000),
  }));

  // Add offline entries for friends without presence records
  const presenceUserIds = new Set(result.map((r: any) => r.user_id));
  for (const fid of friendIds) {
    if (!presenceUserIds.has(fid)) {
      result.push({
        user_id: fid,
        status: "offline",
        status_message: "",
        current_server_id: "",
        last_seen_at: 0,
        updated_at: 0,
      });
    }
  }

  return jsonResponse(result);
});
