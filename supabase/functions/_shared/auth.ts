import { createClient, SupabaseClient } from "https://esm.sh/@supabase/supabase-js@2";

function allowedOrigin(request?: Request): string {
  const configured = Deno.env.get("AMALGAM_ALLOWED_ORIGIN");
  if (configured) return configured;
  // Desktop/native clients do not send a browser Origin. Keep wildcard only
  // for that case; browser deployments should configure an explicit origin.
  const origin = request?.headers.get("origin");
  return origin || "*";
}

export function corsHeadersFor(request?: Request) {
  return {
    "Access-Control-Allow-Origin": allowedOrigin(request),
    "Access-Control-Allow-Headers": "authorization, x-client-info, apikey, content-type, x-whop-signature, webhook-id, webhook-timestamp, webhook-signature",
    "Access-Control-Allow-Methods": "POST, OPTIONS",
    "Content-Type": "application/json",
    "Vary": "Origin",
  };
}

// Kept for existing functions' OPTIONS responses.
export const corsHeaders = corsHeadersFor();

export function jsonResponse(body: unknown, status = 200, request?: Request) {
  return new Response(JSON.stringify(body), { status, headers: corsHeadersFor(request) });
}

export async function readJson(request: Request): Promise<Record<string, unknown>> {
  const contentLength = Number(request.headers.get("content-length") || 0);
  if (contentLength > 1024 * 1024) throw new Error("request body too large");
  const body = await request.json();
  if (!body || typeof body !== "object" || Array.isArray(body)) throw new Error("JSON object required");
  return body as Record<string, unknown>;
}

export async function getAuth(request: Request): Promise<{
  supabase: SupabaseClient;
  userId: string;
  error?: string;
}> {
  const authorization = request.headers.get("Authorization");
  const supabaseUrl = Deno.env.get("SUPABASE_URL");
  const supabaseAnonKey = Deno.env.get("SUPABASE_ANON_KEY");

  if (!authorization?.startsWith("Bearer ") || !supabaseUrl || !supabaseAnonKey) {
    return { supabase: null as any, userId: "", error: "authentication required" };
  }

  const supabase = createClient(supabaseUrl, supabaseAnonKey, {
    global: { headers: { Authorization: authorization } },
  });
  const { data, error } = await supabase.auth.getUser();
  if (error || !data.user) return { supabase, userId: "", error: "invalid session" };
  return { supabase, userId: data.user.id };
}

export function validateLength(value: unknown, name: string, max: number, required = false): string | null {
  if (value === null || value === undefined || value === "") return required ? `${name} is required` : null;
  if (typeof value !== "string") return `${name} must be a string`;
  if (value.length > max) return `${name} must be at most ${max} characters`;
  return null;
}

export function validateRange(value: unknown, name: string, min: number, max: number, required = false): string | null {
  if (value === null || value === undefined || value === "") return required ? `${name} is required` : null;
  const num = Number(value);
  if (!Number.isFinite(num)) return `${name} must be a number`;
  if (num < min || num > max) return `${name} must be between ${min} and ${max}`;
  return null;
}

export function validateUuid(value: unknown, name: string, required = true): string | null {
  if (!value && !required) return null;
  if (!value) return `${name} is required`;
  if (typeof value !== "string") return `${name} must be a string`;
  if (!/^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i.test(value)) return `${name} must be a valid UUID`;
  return null;
}

export function validateEnum(value: unknown, name: string, allowed: string[], required = false): string | null {
  if (!value) return required ? `${name} is required` : null;
  if (typeof value !== "string") return `${name} must be a string`;
  if (!allowed.includes(value)) return `${name} must be one of: ${allowed.join(", ")}`;
  return null;
}

export function sanitize(value: unknown, maxLen: number): string {
  return typeof value === "string" ? value.trim().slice(0, maxLen) : "";
}

export async function enforceRateLimit(supabase: SupabaseClient, action: string, maxPerMinute = 60): Promise<string | null> {
  const { data, error } = await supabase.rpc("enforce_rate_limit", {
    p_action: action,
    p_max_per_minute: maxPerMinute,
  });
  if (error) return "rate limiting unavailable";
  if (data === false || data?.allowed === false) return "rate limit exceeded";
  return null;
}
