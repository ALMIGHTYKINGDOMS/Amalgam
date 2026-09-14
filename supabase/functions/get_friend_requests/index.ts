import { getAuth, jsonResponse } from "../_shared/auth.ts";

Deno.serve(async (request) => {
  if (request.method === "OPTIONS") return new Response("ok", { headers: { "Access-Control-Allow-Origin": "*", "Access-Control-Allow-Headers": "authorization, x-client-info, apikey, content-type" } });
  if (request.method !== "POST") return jsonResponse({ error: "method not allowed" }, 405);

  const { supabase, userId, error } = await getAuth(request);
  if (error) return jsonResponse({ error }, 401);
  const { data: limited } = await supabase.rpc("enforce_rate_limit", { p_action: "get_friend_requests", p_max_per_minute: 60 });
  if (limited === false) return jsonResponse({ error: "rate limit exceeded" }, 429, request);

  try {
    const { data: requests, error: rErr } = await supabase
      .from("friend_requests")
      .select("*")
      .eq("receiver_id", userId)
      .eq("status", "pending")
      .order("created_at", { ascending: false });

    if (rErr) return jsonResponse({ error: rErr.message }, 500);

    if (!requests || requests.length === 0) {
      return jsonResponse([]);
    }

    // Get sender profiles
    const senderIds = requests.map((r: any) => r.sender_id);
    const { data: profiles } = await supabase
      .from("public_profiles")
      .select("user_id, display_name, avatar_url")
      .in("user_id", senderIds);

    const profileMap = new Map((profiles || []).map((p: any) => [p.user_id, p]));

    const result = requests.map((r: any) => {
      const profile = profileMap.get(r.sender_id) || {};
      return {
        id: r.id,
        sender_id: r.sender_id,
        sender_username: profile.display_name || "",
        sender_avatar_url: profile.avatar_url || "",
        receiver_id: r.receiver_id,
        status: r.status,
        message: r.message,
        created_at: Math.floor(new Date(r.created_at).getTime() / 1000),
      };
    });

    return jsonResponse(result);
  } catch (err) {
    return jsonResponse({ error: err.message }, 500);
  }
});
