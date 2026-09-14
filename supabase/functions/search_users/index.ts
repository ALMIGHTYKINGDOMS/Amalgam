import { enforceRateLimit, getAuth, jsonResponse, readJson, sanitize, validateLength, validateRange } from "../_shared/auth.ts";

Deno.serve(async (request) => {
  if (request.method === "OPTIONS") return new Response("ok", { headers: { "Access-Control-Allow-Origin": "*", "Access-Control-Allow-Headers": "authorization, x-client-info, apikey, content-type" } });
  if (request.method !== "POST") return jsonResponse({ error: "method not allowed" }, 405, request);

  const { supabase, userId, error } = await getAuth(request);
  if (error) return jsonResponse({ error }, 401, request);

  try {
    const body = await readJson(request);
    const query = sanitize(body.query, 64);
    const limit = Math.min(Math.max(Number(body.limit || 20), 1), 50);
    const queryError = validateLength(query, "query", 64, true);
    const limitError = validateRange(limit, "limit", 1, 50, true);
    if (queryError || limitError) return jsonResponse({ error: queryError || limitError }, 400, request);
    if (!query) return jsonResponse([], 200, request);

    const limited = await enforceRateLimit(supabase, "search_users", 30);
    if (limited) return jsonResponse({ error: limited }, 429, request);

    const columns = "user_id, display_name, avatar_url, bio, minecraft_username, is_public, friend_count, play_time_seconds, last_seen_at";
    const pattern = `%${query.replace(/[\\%_]/g, "\\$&").replace(/,/g, "")}%`;
    const [displayResult, minecraftResult] = await Promise.all([
      supabase.from("public_profiles").select(columns).eq("is_public", true).neq("user_id", userId).ilike("display_name", pattern).limit(limit),
      supabase.from("public_profiles").select(columns).eq("is_public", true).neq("user_id", userId).ilike("minecraft_username", pattern).limit(limit),
    ]);
    if (displayResult.error || minecraftResult.error) {
      return jsonResponse({ error: displayResult.error?.message || minecraftResult.error?.message }, 400, request);
    }

    const merged = new Map<string, any>();
    for (const row of [...(displayResult.data || []), ...(minecraftResult.data || [])]) merged.set(row.user_id, row);
    return jsonResponse([...merged.values()].slice(0, limit).map((r: any) => ({
      user_id: r.user_id,
      display_name: r.display_name || "",
      avatar_url: r.avatar_url || "",
      bio: r.bio || "",
      minecraft_username: r.minecraft_username || "",
      is_public: r.is_public,
      friend_count: r.friend_count || 0,
      play_time_seconds: r.play_time_seconds || 0,
      last_seen_at: r.last_seen_at ? Math.floor(new Date(r.last_seen_at).getTime() / 1000) : 0,
    })), 200, request);
  } catch (err) {
    return jsonResponse({ error: err instanceof Error ? err.message : "invalid request" }, 400, request);
  }
});
