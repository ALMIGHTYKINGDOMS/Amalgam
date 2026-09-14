import { getAuth, jsonResponse } from "../_shared/auth.ts";

Deno.serve(async (request) => {
  if (request.method === "OPTIONS") return new Response("ok", { headers: { "Access-Control-Allow-Origin": "*", "Access-Control-Allow-Headers": "authorization, x-client-info, apikey, content-type" } });
  if (request.method !== "POST") return jsonResponse({ error: "method not allowed" }, 405);

  const { supabase, userId, error } = await getAuth(request);
  if (error) return jsonResponse({ error }, 401);
  const { data: limited } = await supabase.rpc("enforce_rate_limit", { p_action: "get_friends", p_max_per_minute: 60 });
  if (limited === false) return jsonResponse({ error: "rate limit exceeded" }, 429, request);

  try {
    // Get friendships where user is either A or B
    const { data: friendships, error: fErr } = await supabase
      .from("friendships")
      .select("*")
      .eq("status", "accepted")
      .or(`user_id_a.eq.${userId},user_id_b.eq.${userId}`);

    if (fErr) return jsonResponse({ error: fErr.message }, 500);

    if (!friendships || friendships.length === 0) {
      return jsonResponse([]);
    }

    // Get friend user IDs (the other side of each friendship)
    const friendIds = friendships.map((f: any) =>
      f.user_id_a === userId ? f.user_id_b : f.user_id_a
    );

    // Get profiles for friends
    const { data: profiles } = await supabase
      .from("public_profiles")
      .select("user_id, display_name, avatar_url, minecraft_username")
      .in("user_id", friendIds);

    const profileMap = new Map((profiles || []).map((p: any) => [p.user_id, p]));

    // Merge friendship data with profiles
    const result = friendships.map((f: any) => {
      const friendId = f.user_id_a === userId ? f.user_id_b : f.user_id_a;
      const profile = profileMap.get(friendId) || {};
      return {
        id: f.id,
        user_id_a: f.user_id_a,
        user_id_b: f.user_id_b,
        status: f.status,
        display_name: profile.display_name || "",
        avatar_url: profile.avatar_url || "",
        minecraft_username: profile.minecraft_username || "",
        created_at: Math.floor(new Date(f.created_at).getTime() / 1000),
        updated_at: Math.floor(new Date(f.updated_at).getTime() / 1000),
      };
    });

    return jsonResponse(result);
  } catch (err) {
    return jsonResponse({ error: err.message }, 500);
  }
});
