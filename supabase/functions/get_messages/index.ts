import { getAuth, jsonResponse } from "../_shared/auth.ts";

Deno.serve(async (request) => {
  if (request.method === "OPTIONS") return new Response("ok", { headers: { "Access-Control-Allow-Origin": "*", "Access-Control-Allow-Headers": "authorization, x-client-info, apikey, content-type" } });
  if (request.method !== "POST") return jsonResponse({ error: "method not allowed" }, 405);

  const { supabase, userId, error } = await getAuth(request);
  if (error) return jsonResponse({ error }, 401);
  const { data: limited } = await supabase.rpc("enforce_rate_limit", { p_action: "get_messages", p_max_per_minute: 60 });
  if (limited === false) return jsonResponse({ error: "rate limit exceeded" }, 429, request);

  const body = await request.json();
  const conversationId = body.conversation_id;
  const limit = Math.min(body.limit || 50, 100);
  const offset = body.offset || 0;

  if (!conversationId) return jsonResponse({ error: "conversation_id required" }, 400);

  // Verify user is a participant
  const { data: participation } = await supabase
    .from("conversation_participants")
    .select("conversation_id")
    .eq("conversation_id", conversationId)
    .eq("user_id", userId)
    .single();

  if (!participation) return jsonResponse({ error: "not a participant" }, 403);

  const { data: messages } = await supabase
    .from("messages")
    .select("*")
    .eq("conversation_id", conversationId)
    .order("created_at", { ascending: false })
    .range(offset, offset + limit - 1);

  if (!messages) return jsonResponse([]);

  // Get sender profiles
  const senderIds = [...new Set(messages.map((m: any) => m.sender_id))];
  const { data: profiles } = await supabase
    .from("public_profiles")
    .select("user_id, display_name")
    .in("user_id", senderIds);

  const profileMap = new Map((profiles || []).map((p: any) => [p.user_id, p]));

  const result = messages.reverse().map((m: any) => ({
    id: m.id,
    conversation_id: m.conversation_id,
    sender_id: m.sender_id,
    sender_username: profileMap.get(m.sender_id)?.display_name || "",
    content: m.content,
    is_read: m.is_read,
    created_at: Math.floor(new Date(m.created_at).getTime() / 1000),
  }));

  return jsonResponse(result);
});
