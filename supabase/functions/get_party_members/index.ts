import { getAuth, jsonResponse } from "../_shared/auth.ts";

Deno.serve(async (request) => {
  if (request.method === "OPTIONS") return new Response("ok", { headers: { "Access-Control-Allow-Origin": "*", "Access-Control-Allow-Headers": "authorization, x-client-info, apikey, content-type" } });
  if (request.method !== "POST") return jsonResponse({ error: "method not allowed" }, 405);

  const { supabase, userId, error } = await getAuth(request);
  if (error) return jsonResponse({ error }, 401);
  const { data: limited } = await supabase.rpc("enforce_rate_limit", { p_action: "get_party_members", p_max_per_minute: 60 });
  if (limited === false) return jsonResponse({ error: "rate limit exceeded" }, 429, request);

  const body = await request.json();
  const partyId = body.party_id;
  if (!partyId) return jsonResponse({ error: "party_id required" }, 400);

  // Verify membership
  const { data: membership } = await supabase
    .from("party_members")
    .select("party_id")
    .eq("party_id", partyId)
    .eq("user_id", userId)
    .single();

  if (!membership) return jsonResponse({ error: "not a member" }, 403);

  // Get members
  const { data: members } = await supabase
    .from("party_members")
    .select("*")
    .eq("party_id", partyId);

  if (!members) return jsonResponse([]);

  // Get profiles
  const memberIds = members.map((m: any) => m.user_id);
  const { data: profiles } = await supabase
    .from("public_profiles")
    .select("user_id, display_name, avatar_url")
    .in("user_id", memberIds);

  const profileMap = new Map((profiles || []).map((p: any) => [p.user_id, p]));

  const result = members.map((m: any) => {
    const profile = profileMap.get(m.user_id) || {};
    return {
      party_id: m.party_id,
      user_id: m.user_id,
      username: profile.display_name || "",
      role: m.role,
      joined_at: Math.floor(new Date(m.joined_at).getTime() / 1000),
    };
  });

  return jsonResponse(result);
});
