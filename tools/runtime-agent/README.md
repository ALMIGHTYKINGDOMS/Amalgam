# Amalgam Runtime Node Agent

A standalone Node.js service that hosts Minecraft servers and communicates with the Amalgam Supabase control plane.

## What It Does

1. **Registers** with the Supabase control plane as a hosting node
2. **Heartbeats** every 30 seconds with system metrics (CPU, memory, storage)
3. **Polls** for pending operations (start, stop, restart, command, backup, etc.)
4. **Executes** Minecraft server lifecycle operations by spawning Java processes
5. **Streams** console output to Supabase for remote viewing
6. **Reports** telemetry (TPS, memory, players, entity count) for dashboards

## Architecture

```
┌──────────────────────┐         ┌─────────────────────────┐
│  Amalgam Launcher    │◄───────►│  Supabase Control Plane │
│  (C++ / Windows)     │  API    │  (RLS, Auth, RPCs)      │
└──────────────────────┘         └────────────┬────────────┘
                                              │
                                              │ RPCs
                                              │
                                   ┌──────────▼──────────┐
                                   │  Runtime Node Agent  │
                                   │  (Node.js)           │
                                   │  - Heartbeat         │
                                   │  - Operation polling │
                                   │  - Server management │
                                   │  - Console streaming │
                                   └──────────┬──────────┘
                                              │
                                   ┌──────────▼──────────┐
                                   │  Minecraft Servers   │
                                   │  (Java processes)    │
                                   └─────────────────────┘
```

## Setup

### 1. Provision a hosting node securely

Create the node through the authenticated launcher/API enrollment flow. Store
only the returned node ID and one-time secret in the agent configuration. Do not
insert or log a plaintext `node_secret`; the production migration stores only a
SHA-256 hash.

### 2. Configure the agent

Preferred owner-session setup:

```bash
export SUPABASE_URL="https://nnrrmvaxnoknthpwvttt.supabase.co"
export SUPABASE_ANON_KEY="<supabase-anon-key>"
export SUPABASE_ACCESS_TOKEN="<owner-access-token>"
export SUPABASE_REFRESH_TOKEN="<owner-refresh-token>"  # recommended for long-running agents
export AMALGAM_NODE_ID="<hosting-node-uuid>"
export AMALGAM_NODE_SECRET="<random-secret-from-enrollment>"
```

Config file (`~/.amalgam/runtime/agent.json`) equivalent:

```json
{
  "supabaseUrl": "https://nnrrmvaxnoknthpwvttt.supabase.co",
  "supabaseKey": "<supabase-anon-key>",
  "supabaseAccessToken": "<owner-access-token>",
  "nodeId": "<hosting-node-uuid>",
  "nodeSecret": "<random-secret-from-enrollment>",
  "nodeName": "My Server Node",
  "region": "us-east-1"
}
```

A narrowly scoped service integration may use a pre-provisioned `nodeId`, but
must never be used to auto-register arbitrary nodes. The agent refuses an
unscoped startup.

### 3. Run the agent

```bash
cd tools/runtime-agent
node src/index.js
```

Or with npm:

```bash
npm start
```

### 4. Create a server instance

From the C++ launcher or via Supabase:

```sql
-- First create a server_instances record
SELECT create_server_from_template(
  '<template-uuid>',
  '<node-uuid>',
  'My Minecraft Server',
  '<user-uuid>',
  'my-server',
  25565
);
```

### 5. Deploy a server jar

Place a `server.jar` in the server's data directory:

```
~/.amalgam/runtime/servers/my_server/server.jar
```

### 6. Start the server

From the launcher UI or via an operation:

```sql
INSERT INTO server_operations (server_id, user_id, operation, params)
VALUES ('<server-uuid>', '<user-uuid>', 'start', '{}');
```

The agent will pick it up within 5 seconds and start the Minecraft server.

## Supported Operations

| Operation | Description |
|-----------|-------------|
| `start` | Start the Minecraft server |
| `stop` | Gracefully stop (sends `stop` command) |
| `restart` | Stop then start |
| `command` | Send a console command |
| `backup` | Copy world directory to backups folder |
| `restore` | Restore from a backup |
| `settings` | Write server.properties |
| `install_jar` | Download a server jar from URL |
| `delete` | Stop server and remove all data |

## Monitoring

- Console output is streamed to `console_lines` table every 5 seconds
- Telemetry (TPS, memory, players) is reported to `runtime_telemetry` every 60 seconds
- Node health is reported via heartbeat every 30 seconds

## Security

- The agent authenticates with a pre-shared `node_secret` (not a user JWT)
- RLS policies ensure the agent can only write to servers it owns
- The launcher (browser/app) never has direct database access
- All privileged writes go through RPCs with `SECURITY DEFINER`
