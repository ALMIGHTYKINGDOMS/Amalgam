import { getAuth, jsonResponse, readJson, sanitize, validateLength, validateUuid } from "../_shared/auth.ts";

Deno.serve(async (request) => {
  if (request.method === "OPTIONS") return new Response("ok", { headers: { "Access-Control-Allow-Origin": "*", "Access-Control-Allow-Headers": "authorization, x-client-info, apikey, content-type" } });
  if (request.method !== "POST") return jsonResponse({ error: "method not allowed" }, 405, request);
  const { supabase, userId, error } = await getAuth(request);
  if (error) return jsonResponse({ error }, 401, request);
  const { data: limited } = await supabase.rpc("enforce_rate_limit", { p_action: "send_message", p_max_per_minute: 30 });
  if (limited === false) return jsonResponse({ error: "rate limit exceeded" }, 429, request);
  try {
    const body = await readJson(request);
    const conversationError = validateUuid(body.conversation_id, "conversation_id");
    const content = sanitize(body.content, 4000);
    const contentError = validateLength(content, "content", 4000, true);
    if (conversationError || contentError) return jsonResponse({ error: conversationError || contentError }, 400, request);
    const { data: message, error: rpcError } = await supabase.rpc("send_conversation_message", {
      p_conversation_id: body.conversation_id,
      p_content: content,
    });
    if (rpcError) return jsonResponse({ error: rpcError.message }, 400, request);
    return jsonResponse({
      id: message?.id,
      conversation_id: body.conversation_id,
      sender_id: userId,
      content: message?.content || content,
      is_read: false,
      created_at: message?.created_at ? Math.floor(new Date(message.created_at).getTime() / 1000) : Math.floor(Date.now() / 1000),
    }, 201, request);
  } catch (err) {
    return jsonResponse({ error: err instanceof Error ? err.message : "invalid request" }, 400, request);
  }
});
