# Cloudflare TURN Integration

Amalgam uses Supabase only for authenticated signaling and short-lived TURN
credential issuance. Minecraft game traffic is never sent through Supabase.

## Deployment Secrets

Configure these as Supabase Edge Function secrets after rotating any exposed
Cloudflare token:

```text
CF_TURN_TOKEN_ID=<Cloudflare TURN token ID>
CF_TURN_API_TOKEN=<rotated Cloudflare API token>
```

Deploy `supabase/functions/get-turn-credentials/index.ts` with JWT verification
enabled. The function authenticates the Supabase user, requests a credential
with a maximum one-hour TTL from Cloudflare, and returns only the temporary ICE
server response.

## Connection Order

1. Authenticate through Supabase.
2. Exchange session metadata and join token through Supabase.
3. Attempt direct peer/LAN connectivity.
4. Request temporary Cloudflare TURN credentials only if direct connectivity fails.
5. Use TURN for peer transport only; Supabase remains out of the game-data path.

TURN is a relay fallback and may incur Cloudflare usage charges. It is not
required for local sessions or direct connections.

## Native TCP Bridge

The launcher uses `libdatachannel` to carry a dedicated Minecraft server's
local TCP stream over a reliable WebRTC DataChannel. The host adapter connects
to `127.0.0.1:<server-port>`; the guest adapter listens on an ephemeral
loopback port and the guest Minecraft process is launched against that port.
SDP and ICE candidates are exchanged through the `essentials_signals` table.
Apply migration `202608170003_essentials_signaling.sql` before enabling this
path. No Minecraft packets are modified and no game bytes are stored in
Supabase.
