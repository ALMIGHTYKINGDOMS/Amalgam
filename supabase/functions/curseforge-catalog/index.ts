import {
  corsHeadersFor,
  enforceRateLimit,
  getAuth,
  jsonResponse,
  readJson,
} from "../_shared/auth.ts";

const CURSEFORGE_ORIGIN = "https://api.curseforge.com";
const MAX_RESPONSE_BYTES = 4 * 1024 * 1024;
const ALLOWED_CLASS_IDS = new Set([6, 12, 4471, 4559, 6552, 6945]);
const ALLOWED_LOADER_IDS = new Set([0, 1, 4, 5, 6]);
const SEARCH_PARAMS = new Set([
  "gameId",
  "classId",
  "searchFilter",
  "sortField",
  "sortOrder",
  "pageSize",
  "index",
  "modLoaderType",
  "gameVersion",
]);

function integerParam(url: URL, name: string, min: number, max: number): boolean {
  const value = url.searchParams.get(name);
  if (value === null) return true;
  if (!/^\d+$/.test(value)) return false;
  const parsed = Number(value);
  return Number.isSafeInteger(parsed) && parsed >= min && parsed <= max;
}

function validateSearch(url: URL): string | null {
  for (const key of url.searchParams.keys()) {
    if (!SEARCH_PARAMS.has(key)) return `unsupported search parameter: ${key}`;
  }
  if (url.searchParams.get("gameId") !== "432") return "only Minecraft catalog requests are allowed";
  const classId = Number(url.searchParams.get("classId") || 0);
  if (!ALLOWED_CLASS_IDS.has(classId)) return "unsupported Minecraft content class";
  if (!integerParam(url, "pageSize", 1, 50) || !integerParam(url, "index", 0, 5000) ||
      !integerParam(url, "sortField", 1, 6)) return "invalid pagination or sort value";
  const query = url.searchParams.get("searchFilter") || "";
  if (query.length > 100) return "search query is too long";
  const gameVersion = url.searchParams.get("gameVersion") || "";
  if (gameVersion.length > 32 || !/^[0-9A-Za-z.+_-]*$/.test(gameVersion)) return "invalid game version";
  const loader = Number(url.searchParams.get("modLoaderType") || 0);
  if (!ALLOWED_LOADER_IDS.has(loader)) return "unsupported loader";
  const order = url.searchParams.get("sortOrder");
  if (order !== null && order !== "asc" && order !== "desc") return "invalid sort order";
  return null;
}

function allowedProviderUrl(path: unknown): { url?: URL; error?: string } {
  if (typeof path !== "string" || path.length < 2 || path.length > 1024 || !path.startsWith("/v1/")) {
    return { error: "a valid provider path is required" };
  }
  let url: URL;
  try {
    url = new URL(path, CURSEFORGE_ORIGIN);
  } catch {
    return { error: "invalid provider path" };
  }
  if (url.origin !== CURSEFORGE_ORIGIN) return { error: "provider origin is not allowed" };

  if (url.pathname === "/v1/mods/search") {
    const searchError = validateSearch(url);
    return searchError ? { error: searchError } : { url };
  }
  const permitted = [
    /^\/v1\/mods\/\d+$/,
    /^\/v1\/mods\/\d+\/description$/,
    /^\/v1\/mods\/\d+\/files$/,
    /^\/v1\/mods\/\d+\/files\/\d+$/,
    /^\/v1\/mods\/\d+\/files\/\d+\/download-url$/,
  ].some((pattern) => pattern.test(url.pathname));
  if (!permitted || [...url.searchParams.keys()].length > 0) {
    return { error: "provider path is not allowed" };
  }
  return { url };
}

Deno.serve(async (request) => {
  if (request.method === "OPTIONS") {
    return new Response("ok", { headers: corsHeadersFor(request) });
  }
  if (request.method !== "POST") return jsonResponse({ error: "method not allowed" }, 405, request);

  const { supabase, error: authError } = await getAuth(request);
  if (authError) return jsonResponse({ error: authError }, 401, request);
  const rateError = await enforceRateLimit(supabase, "curseforge_catalog", 120);
  if (rateError) return jsonResponse({ error: rateError }, rateError.includes("exceeded") ? 429 : 503, request);

  let body: Record<string, unknown>;
  try {
    body = await readJson(request);
  } catch (error) {
    return jsonResponse({ error: error instanceof Error ? error.message : "invalid request" }, 400, request);
  }
  const target = allowedProviderUrl(body.path);
  if (!target.url) return jsonResponse({ error: target.error || "provider path rejected" }, 400, request);

  const providerKey = Deno.env.get("CURSEFORGE_API_KEY")?.trim();
  if (!providerKey) return jsonResponse({ error: "CurseForge catalog is not configured" }, 503, request);

  let upstream: Response;
  try {
    upstream = await fetch(target.url, {
      headers: {
        "Accept": "application/json",
        "User-Agent": "AmalgamLauncher/1.0",
        "x-api-key": providerKey,
      },
      signal: AbortSignal.timeout(15_000),
    });
  } catch {
    return jsonResponse({ error: "CurseForge catalog is temporarily unavailable" }, 502, request);
  }

  const text = await upstream.text();
  if (!upstream.ok) {
    return jsonResponse({ error: "CurseForge request failed", provider_status: upstream.status }, 502, request);
  }
  if (new TextEncoder().encode(text).byteLength > MAX_RESPONSE_BYTES) {
    return jsonResponse({ error: "CurseForge response exceeded the safety limit" }, 502, request);
  }
  try {
    JSON.parse(text);
  } catch {
    return jsonResponse({ error: "CurseForge returned an invalid response" }, 502, request);
  }

  const headers = {
    ...corsHeadersFor(request),
    "Cache-Control": "private, max-age=60",
  };
  return new Response(text, { status: 200, headers });
});
