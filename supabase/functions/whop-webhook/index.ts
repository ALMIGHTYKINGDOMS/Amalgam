import { createClient } from "https://esm.sh/@supabase/supabase-js@2";

const cors = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Headers": "content-type, webhook-id, webhook-timestamp, webhook-signature, x-whop-signature",
  "Content-Type": "application/json",
};
const UUID_RE = /^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i;

function json(body: unknown, status = 200) {
  return new Response(JSON.stringify(body), { status, headers: cors });
}

function decodeBase64(value: string): Uint8Array {
  const normalized = value.replace(/-/g, "+").replace(/_/g, "/").padEnd(Math.ceil(value.length / 4) * 4, "=");
  const binary = atob(normalized);
  return Uint8Array.from(binary, (char) => char.charCodeAt(0));
}

async function verifySignature(request: Request, rawBody: string, secret: string): Promise<string> {
  const webhookId = request.headers.get("webhook-id") || "";
  const timestamp = request.headers.get("webhook-timestamp") || "";
  const signatureHeader = request.headers.get("webhook-signature") || request.headers.get("x-whop-signature") || "";
  if (!webhookId || !timestamp || !signatureHeader) throw new Error("missing webhook signature headers");

  const timestampNumber = Number(timestamp);
  if (!Number.isFinite(timestampNumber) || Math.abs(Date.now() / 1000 - timestampNumber) > 300) {
    throw new Error("webhook timestamp outside replay window");
  }

  const secretBytes = secret.startsWith("whsec_") ? decodeBase64(secret.slice(6)) : new TextEncoder().encode(secret);
  const key = await crypto.subtle.importKey("raw", secretBytes, { name: "HMAC", hash: "SHA-256" }, false, ["verify"]);
  const signedPayload = `${webhookId}.${timestamp}.${rawBody}`;
  const signatures = signatureHeader.split(/\s+/).map((part) => part.includes(",") ? part.split(",", 2)[1] : part);
  for (const signature of signatures) {
    try {
      if (await crypto.subtle.verify("HMAC", key, decodeBase64(signature), new TextEncoder().encode(signedPayload))) return webhookId;
    } catch {
      // Ignore malformed candidate signatures and check the remaining values.
    }
  }
  throw new Error("invalid webhook signature");
}

function extractUuid(value: unknown): string {
  return typeof value === "string" && UUID_RE.test(value) ? value : "";
}

Deno.serve(async (request) => {
  if (request.method === "OPTIONS") return new Response("ok", { headers: cors });
  if (request.method !== "POST") return json({ error: "method not allowed" }, 405);

  const supabaseUrl = Deno.env.get("SUPABASE_URL");
  const serviceKey = Deno.env.get("SUPABASE_SERVICE_ROLE_KEY");
  const webhookSecret = Deno.env.get("WHOP_WEBHOOK_SECRET");
  if (!supabaseUrl || !serviceKey || !webhookSecret) return json({ error: "webhook is not configured" }, 503);

  const rawBody = await request.text();
  let event: any;
  let eventId = "";
  try {
    eventId = await verifySignature(request, rawBody, webhookSecret);
    event = JSON.parse(rawBody);
  } catch (error) {
    return json({ error: error instanceof Error ? error.message : "invalid webhook" }, 401);
  }

  const eventType = String(event.type || event.event_type || "").slice(0, 128);
  const data = event.data || event.payload || {};
  const supabase = createClient(supabaseUrl, serviceKey, { auth: { autoRefreshToken: false, persistSession: false } });

  const { error: eventInsertError } = await supabase.from("webhook_events").insert({
    provider: "whop",
    event_id: eventId,
    event_type: eventType,
    payload: event,
  });
  if (eventInsertError?.code === "23505") return json({ received: true, duplicate: true });
  if (eventInsertError) return json({ error: "could not record webhook" }, 500);

  try {
    const metadata = data.metadata || data.custom_data || {};
    let userId = extractUuid(metadata.amalgam_user_id || metadata.user_id || data.amalgam_user_id);
    const customerId = String(data.customer_id || data.user_id || data.membership_id || "").slice(0, 255);
    if (!userId && customerId) {
      const { data: mapping } = await supabase.from("whop_customer_mappings").select("user_id").eq("whop_customer_id", customerId).maybeSingle();
      userId = extractUuid(mapping?.user_id);
    }

    if (!userId) {
      await supabase.from("webhook_events").update({ processed_at: new Date().toISOString() }).eq("provider", "whop").eq("event_id", eventId);
      return json({ received: true, skipped: true, reason: "no Amalgam user mapping" }, 202);
    }

    const externalId = String(data.subscription_id || data.id || eventId).slice(0, 255);
    const planId = String(data.plan_id || data.product_id || "free").slice(0, 128);
    const rawStatus = String(data.status || "active").toLowerCase();
    const mappedStatus = ["cancelled", "canceled"].includes(rawStatus) ? "cancelled"
      : ["past_due", "trialing", "expired", "incomplete"].includes(rawStatus) ? rawStatus : "active";
    const amount = Number(data.amount || data.price || 0);
    const periodStart = Number(data.current_period_start || 0);
    const periodEnd = Number(data.current_period_end || 0);

    const { error: upsertError } = await supabase.from("subscriptions").upsert({
      user_id: userId,
      plan_id: planId,
      plan_label: planId.toUpperCase().slice(0, 128),
      status: mappedStatus,
      amount: Number.isFinite(amount) ? amount : 0,
      currency: String(data.currency || "USD").slice(0, 8),
      current_period_start: Number.isFinite(periodStart) ? periodStart : 0,
      current_period_end: Number.isFinite(periodEnd) ? periodEnd : 0,
      cancelled_at: mappedStatus === "cancelled" ? Math.floor(Date.now() / 1000) : 0,
      provider: "whop",
      external_id: externalId,
      updated_at: new Date().toISOString(),
    }, { onConflict: "provider,external_id" });
    if (upsertError) throw new Error(upsertError.message);

    await supabase.from("webhook_events").update({ processed_at: new Date().toISOString() }).eq("provider", "whop").eq("event_id", eventId);
    await supabase.rpc("record_audit", {
      p_action: `subscription.${eventType}`,
      p_resource_type: "subscription",
      p_resource_id: externalId,
      p_metadata: { user_id: userId, plan_id: planId, status: mappedStatus },
      p_source: "whop-webhook",
    });
    return json({ received: true });
  } catch (error) {
    console.error("[whop-webhook] processing error", error);
    return json({ error: "webhook processing failed" }, 500);
  }
});
