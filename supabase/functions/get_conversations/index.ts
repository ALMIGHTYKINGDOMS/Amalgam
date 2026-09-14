import { getAuth, jsonResponse } from "../_shared/auth.ts";

Deno.serve(async (request) => {
  if (request.method === "OPTIONS") return new Response("ok", { headers: { "Access-Control-Allow-Origin": "*", "Access-Control-Allow-Headers": "authorization, x-client-info, apikey, content-type" } });
  if (request.method !== "POST") return jsonResponse({ error: "method not allowed" }, 405);

  const { supabase, userId, error } = await getAuth(request);
  if (error) return jsonResponse({ error }, 401);
  const { data: limited } = await supabase.rpc("enforce_rate_limit", { p_action: "get_conversations", p_max_per_minute: 30 });
  if (limited === false) return jsonResponse({ error: "rate limit exceeded" }, 429, request);

  // Get conversation IDs the user participates in
  const { data: participations } = await supabase
    .from("conversation_participants")
    .select("conversation_id, last_read_at")
    .eq("user_id", userId);

  if (!participations || participations.length === 0) return jsonResponse([]);

  const convIds = participations.map((p: any) => p.conversation_id);
  const readMap = new Map(participations.map((p: any) => [p.conversation_id, p.last_read_at]));

  // Get conversations
  const { data: conversations } = await supabase
    .from("conversations")
    .select("*")
    .in("id", convIds)
    .order("last_message_at", { ascending: false });

  if (!conversations) return jsonResponse([]);

  // Get other participants' profiles
  const { data: allParticipants } = await supabase
    .from("conversation_participants")
    .select("conversation_id, user_id")
    .in("conversation_id", convIds)
    .neq("user_id", userId);

  const otherUserMap = new Map<string, string>();
  (allParticipants || []).forEach((p: any) => otherUserMap.set(p.conversation_id, p.user_id));

  const otherUserIds = [...new Set((allParticipants || []).map((p: any) => p.user_id))];
  const { data: profiles } = await supabase
    .from("public_profiles")
    .select("user_id, display_name, avatar_url")
    .in("user_id", otherUserIds);

  const profileMap = new Map((profiles || []).map((p: any) => [p.user_id, p]));

  // Count unread messages per conversation
  const result = [];
  for (const conv of conversations) {
    const otherUserId = otherUserMap.get(conv.id) || "";
    const profile = profileMap.get(otherUserId) || {};
    const lastRead = readMap.get(conv.id);

    let unreadCount = 0;
    if (lastRead) {
      const { count } = await supabase
        .from("messages")
        .select("*", { count: "exact", head: true })
        .eq("conversation_id", conv.id)
        .neq("sender_id", userId)
        .gt("created_at", lastRead);
      unreadCount = count || 0;
    } else {
      const { count } = await supabase
        .from("messages")
        .select("*", { count: "exact", head: true })
        .eq("conversation_id", conv.id)
        .neq("sender_id", userId);
      unreadCount = count || 0;
    }

    result.push({
      id: conv.id,
      participant_ids: [userId, otherUserId],
      last_message_content: conv.last_message_content,
      last_message_sender_id: conv.last_message_sender_id,
      last_message_at: conv.last_message_at ? Math.floor(new Date(conv.last_message_at).getTime() / 1000) : 0,
      created_at: Math.floor(new Date(conv.created_at).getTime() / 1000),
      unread_count: unreadCount,
    });
  }

  return jsonResponse(result);
});
