import { getAuth, jsonResponse, validateUuid } from "../_shared/auth.ts";

const cors = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Headers": "authorization, x-client-info, apikey, content-type",
  "Content-Type": "application/json",
};

Deno.serve(async (request) => {
  if (request.method === "OPTIONS") return new Response("ok", { headers: cors });
  if (request.method !== "POST") return jsonResponse({ error: "method not allowed" }, 405);

  const { supabase, userId, error } = await getAuth(request);
  if (error) return jsonResponse({ error }, 401);
  const { data: limited } = await supabase.rpc("enforce_rate_limit", { p_action: "get_or_create_conversation", p_max_per_minute: 20 });
  if (limited === false) return jsonResponse({ error: "rate limit exceeded" }, 429, request);

  try {
    const body = await request.json();
    const otherUserId = body.other_user_id;
    const validation = validateUuid(otherUserId, "other_user_id");
    if (validation) return jsonResponse({ error: validation }, 400);
    if (otherUserId === userId) return jsonResponse({ error: "cannot create conversation with yourself" }, 400);

    const { data, error: rpcError } = await supabase.rpc("create_direct_conversation", {
      p_other_user_id: otherUserId,
    });
    if (rpcError) return jsonResponse({ error: rpcError.message }, 400);

    return jsonResponse({
      id: data?.id,
      participant_ids: data?.participant_ids || [userId, otherUserId],
      created: true,
      created_at: Math.floor(Date.now() / 1000),
    });
  } catch (err) {
    return jsonResponse({ error: err instanceof Error ? err.message : "invalid request" }, 400);
  }
});
