import { getAuth, jsonResponse } from "../_shared/auth.ts";

Deno.serve(async (request) => {
  if (request.method === "OPTIONS") return new Response("ok", { headers: { "Access-Control-Allow-Origin": "*", "Access-Control-Allow-Headers": "authorization, x-client-info, apikey, content-type" } });
  if (request.method !== "POST") return jsonResponse({ error: "method not allowed" }, 405);

  const { supabase, userId, error } = await getAuth(request);
  if (error) return jsonResponse({ error }, 401);
  const { data: limited } = await supabase.rpc("enforce_rate_limit", { p_action: "get_parties", p_max_per_minute: 30 });
  if (limited === false) return jsonResponse({ error: "rate limit exceeded" }, 429, request);

  // Get party IDs the user is a member of
  const { data: memberships } = await supabase
    .from("party_members")
    .select("party_id")
    .eq("user_id", userId);

  if (!memberships || memberships.length === 0) return jsonResponse([]);

  const partyIds = memberships.map((m: any) => m.party_id);

  const { data: parties } = await supabase
    .from("parties")
    .select("*")
    .in("id", partyIds)
    .order("created_at", { ascending: false });

  if (!parties) return jsonResponse([]);

  const result = [];
  for (const party of parties) {
    const { count: memberCount } = await supabase
      .from("party_members")
      .select("*", { count: "exact", head: true })
      .eq("party_id", party.id);

    const { data: ownerProfile } = await supabase
      .from("public_profiles")
      .select("display_name")
      .eq("user_id", party.owner_id)
      .single();

    result.push({
      id: party.id,
      owner_id: party.owner_id,
      owner_username: ownerProfile?.display_name || "",
      name: party.name,
      is_public: party.is_public,
      invite_code: party.invite_code,
      max_members: party.max_members,
      member_count: memberCount || 0,
      created_at: Math.floor(new Date(party.created_at).getTime() / 1000),
    });
  }

  return jsonResponse(result);
});
