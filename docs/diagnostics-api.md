# Diagnostics Upload Contract

Remote upload is not enabled by the client yet. This document fixes the contract so a future server can be implemented without changing the local consent or storage model.

## Endpoint

- Scheme: `https` only; HTTP, embedded credentials, redirects to HTTP, and client-supplied server certificates are rejected.
- Path: `/v1/diagnostics`
- Content type: `application/json`
- Authentication: short-lived server-issued upload token supplied out-of-band by the beta service. Never ship a shared ingestion secret in the launcher or DLL.

## Payload

The payload is an array of aggregate match summaries. It contains no raw sidebar text, names, coordinates, entities, actions, chat, inventory, screenshots, server address, mod jars, or credentials.

```json
{
  "schema": 1,
  "client": "amalgam",
  "items": [
    {
      "match_id": 7,
      "mode": "Practice",
      "confidence": 90,
      "elapsed_ms": 12000,
      "players_alive": -1,
      "teams_alive": -1,
      "kills": 3,
      "final_kills": 1,
      "beds_alive": -1,
      "episode": -1,
      "crystals": 0,
      "explosions": 4
    }
  ]
}
```

The server must validate the schema, bounds, item count, and numeric ranges. It must not treat `match_id` as a globally unique user identifier.

## Delivery

- Upload only after explicit beta consent and a preview of the aggregate fields.
- Mark a local row delivered only after a `2xx` response with a server receipt id.
- Retry with exponential backoff for transport errors and `408`/`429`/`5xx`; do not retry malformed payloads or `4xx` policy errors.
- Keep the local outbox bounded and allow delete/export at any time.
- The server must support account deletion and retention expiry without requiring a client secret.
