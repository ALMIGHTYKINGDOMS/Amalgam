#!/usr/bin/env node
// Amalgam Runtime Agent: node registration, heartbeat, operations, console,
// telemetry, and durable event collection.

import { createClient } from "@supabase/supabase-js";
import { registerNode, sendHeartbeat, heartbeatLoop } from "./heartbeat.js";
import { operationPollLoop, setEventCollector } from "./operations.js";
import { startFlushLoop } from "./console.js";
import { loadConfig, ensureDataDir, printUsage, validateConfig } from "./config.js";
import { ServerProcessManager } from "./server-lifecycle.js";
import { EventCollector } from "./events.js";
import os from "node:os";

const VERSION = "0.2.0";

async function createSupabase(config) {
  const options = {
    auth: { autoRefreshToken: Boolean(config.supabaseRefreshToken), persistSession: false, detectSessionInUrl: false },
  };
  if (config.supabaseAccessToken) {
    options.global = { headers: { Authorization: `Bearer ${config.supabaseAccessToken}` } };
  }
  const client = createClient(config.supabaseUrl, config.supabaseKey, options);
  if (config.supabaseAccessToken && config.supabaseRefreshToken) {
    const { error } = await client.auth.setSession({
      access_token: config.supabaseAccessToken,
      refresh_token: config.supabaseRefreshToken,
    });
    if (error) console.warn(`[agent] Session refresh is unavailable: ${error.message}`);
  }
  return client;
}

async function main() {
  const config = loadConfig();
  if (config.help) {
    printUsage();
    return;
  }

  ensureDataDir(config);
  const configErrors = validateConfig(config);
  if (configErrors.length) {
    for (const error of configErrors) console.error(`[agent] FATAL: ${error}`);
    process.exitCode = 1;
    return;
  }

  console.log(`[agent] Amalgam Runtime Agent v${VERSION}`);
  console.log(`[agent] Supabase URL: ${config.supabaseUrl}`);
  console.log(`[agent] Node name:   ${config.nodeName}`);
  console.log(`[agent] Region:      ${config.region}`);
  console.log(`[agent] Host:        ${config.host || os.hostname()}`);
  console.log(`[agent] Data dir:    ${config.dataDir}`);

  const supabase = await createSupabase(config);
  let nodeInfo;

  if (config.nodeId) {
    nodeInfo = { id: config.nodeId, reregistered: false, status: "provisioned" };
    // Verify the pre-provisioned node before starting worker loops.
    const healthy = await sendHeartbeat(supabase, nodeInfo.id, config.nodeSecret, 0);
    if (!healthy) {
      console.error("[agent] FATAL: provisioned node credentials were rejected");
      process.exitCode = 1;
      return;
    }
  } else {
    console.log("[agent] Enrolling node with the authenticated owner session...");
    nodeInfo = await registerNode(supabase, config);
    if (!nodeInfo.id) {
      console.error("[agent] FATAL: failed to enroll node; provide --node-id for a pre-provisioned node");
      process.exitCode = 1;
      return;
    }
  }

  console.log(`[agent] Registered as node ${nodeInfo.id}`);
  const events = new EventCollector(supabase, config, nodeInfo.id);
  events.start();
  events.registrationSucceeded(nodeInfo.id, nodeInfo.reregistered);
  setEventCollector(events);

  const manager = new ServerProcessManager(config, events);
  let running = true;
  let shutdownPromise = null;

  const shutdown = (signal) => {
    if (shutdownPromise) return shutdownPromise;
    running = false;
    shutdownPromise = (async () => {
      console.log(`\n[agent] Received ${signal}, shutting down...`);
      events.event("server", "agent_shutdown", { signal });
      await manager.shutdown();
      await events.stop();
      try {
        await supabase.rpc("node_heartbeat", {
          p_node_id: nodeInfo.id,
          p_node_secret: config.nodeSecret,
          p_status: "offline",
        });
        console.log("[agent] Marked node as offline");
      } catch (error) {
        console.warn(`[agent] Could not mark node offline: ${error.message}`);
      }
    })();
    return shutdownPromise;
  };

  const onSignal = (signal) => {
    void shutdown(signal).finally(() => process.exit(0));
  };
  process.once("SIGINT", () => onSignal("SIGINT"));
  process.once("SIGTERM", () => onSignal("SIGTERM"));

  const isRunning = () => running;
  events.event("server", "agent_started", {
    node_id: nodeInfo.id,
    node_name: config.nodeName,
    region: config.region,
    version: VERSION,
  });

  // These are independent long-lived loops. They must start concurrently;
  // awaiting operationPollLoop here would prevent console/telemetry startup.
  heartbeatLoop(supabase, nodeInfo, config, isRunning, events, () => manager.servers.size);
  startFlushLoop(supabase, nodeInfo, config, manager, isRunning, events);
  void operationPollLoop(supabase, nodeInfo, config, isRunning, manager)
    .catch((error) => {
      console.error("[ops] Fatal poll-loop error:", error);
      events.error("operation", error.message, { component: "OperationPollLoop", severity: "fatal" });
      void shutdown("operation-loop");
    });

  console.log("[agent] Heartbeat, operations, console, telemetry, and events are running.");
  while (running) await new Promise((resolve) => setTimeout(resolve, 1000));
}

main().catch((error) => {
  console.error("[agent] Fatal error:", error);
  process.exitCode = 1;
});
