// Runtime agent configuration.
// Priority: CLI flags > environment variables > config file > defaults.

import fs from "node:fs";
import path from "node:path";
import os from "node:os";

const DEFAULTS = {
  supabaseUrl: "",
  // Prefer the public/anon key plus a short-lived owner access token. A
  // service-role key is accepted only for pre-provisioned node operation.
  supabaseKey: "",
  supabaseAccessToken: "",
  supabaseRefreshToken: "",
  nodeId: "",
  nodeSecret: "",
  nodeName: os.hostname(),
  region: "us-east-1",
  host: "",
  dataDir: path.join(os.homedir(), ".amalgam", "runtime"),
  heartbeatIntervalSec: 30,
  operationPollIntervalSec: 5,
  telemetryIntervalSec: 60,
  maxConcurrentOps: 4,
  javaPath: "java",
  help: false,
};

function positiveInt(value, fallback) {
  const parsed = Number.parseInt(value, 10);
  return Number.isFinite(parsed) && parsed > 0 ? parsed : fallback;
}

export function loadConfig() {
  const config = { ...DEFAULTS };

  const envMap = {
    SUPABASE_URL: "supabaseUrl",
    SUPABASE_ANON_KEY: "supabaseKey",
    // Backwards-compatible name. Do not use it to auto-register a node.
    SUPABASE_SERVICE_KEY: "supabaseKey",
    SUPABASE_ACCESS_TOKEN: "supabaseAccessToken",
    SUPABASE_REFRESH_TOKEN: "supabaseRefreshToken",
    AMALGAM_NODE_ID: "nodeId",
    AMALGAM_NODE_SECRET: "nodeSecret",
    AMALGAM_NODE_NAME: "nodeName",
    AMALGAM_REGION: "region",
    AMALGAM_HOST: "host",
    AMALGAM_DATA_DIR: "dataDir",
    AMALGAM_JAVA_PATH: "javaPath",
  };

  for (const [envKey, configKey] of Object.entries(envMap)) {
    if (process.env[envKey]) config[configKey] = process.env[envKey];
  }

  const args = process.argv.slice(2);
  for (let i = 0; i < args.length; i++) {
    const arg = args[i];
    switch (arg) {
      case "--help":
      case "-h":
        config.help = true;
        break;
      case "--url": config.supabaseUrl = args[++i] || ""; break;
      case "--key": config.supabaseKey = args[++i] || ""; break;
      case "--token": config.supabaseAccessToken = args[++i] || ""; break;
      case "--refresh-token": config.supabaseRefreshToken = args[++i] || ""; break;
      case "--node-id": config.nodeId = args[++i] || ""; break;
      case "--secret": config.nodeSecret = args[++i] || ""; break;
      case "--name": config.nodeName = args[++i] || config.nodeName; break;
      case "--region": config.region = args[++i] || config.region; break;
      case "--host": config.host = args[++i] || ""; break;
      case "--data-dir": config.dataDir = args[++i] || config.dataDir; break;
      case "--java": config.javaPath = args[++i] || config.javaPath; break;
      case "--heartbeat-interval": config.heartbeatIntervalSec = positiveInt(args[++i], 30); break;
      case "--poll-interval": config.operationPollIntervalSec = positiveInt(args[++i], 5); break;
      case "--telemetry-interval": config.telemetryIntervalSec = positiveInt(args[++i], 60); break;
    }
  }

  const configPath = path.join(config.dataDir, "agent.json");
  if (fs.existsSync(configPath)) {
    try {
      const fileConfig = JSON.parse(fs.readFileSync(configPath, "utf-8"));
      for (const [key, value] of Object.entries(fileConfig)) {
        if (!config[key] || config[key] === DEFAULTS[key]) config[key] = value;
      }
    } catch {
      // Malformed local config is reported by validation in the entry point.
    }
  }

  config.heartbeatIntervalSec = positiveInt(config.heartbeatIntervalSec, 30);
  config.operationPollIntervalSec = positiveInt(config.operationPollIntervalSec, 5);
  config.telemetryIntervalSec = positiveInt(config.telemetryIntervalSec, 60);
  config.maxConcurrentOps = Math.min(32, positiveInt(config.maxConcurrentOps, 4));
  return config;
}

export function validateConfig(config) {
  const errors = [];
  if (!config.supabaseUrl) errors.push("SUPABASE_URL is required");
  if (!config.supabaseKey) errors.push("SUPABASE_ANON_KEY (or legacy SUPABASE_SERVICE_KEY) is required");
  if (!config.nodeSecret) errors.push("AMALGAM_NODE_SECRET is required");
  if (!config.nodeId && !config.supabaseAccessToken) {
    errors.push("AMALGAM_NODE_ID is required for a pre-provisioned node, or SUPABASE_ACCESS_TOKEN is required to enroll one");
  }
  if (config.supabaseUrl && !/^https:\/\//i.test(config.supabaseUrl)) {
    errors.push("SUPABASE_URL must use HTTPS");
  }
  return errors;
}

export function printUsage() {
  console.log(`
Amalgam Runtime Node Agent

USAGE:
  amalgam-runtime [OPTIONS]

OPTIONS:
  --url <url>                  Supabase project URL (HTTPS)
  --key <key>                  Supabase anon key; legacy service key is accepted for a provisioned node
  --token <jwt>                Owner access token for authenticated RPCs/enrollment
  --refresh-token <token>      Refresh token for long-running owner sessions
  --node-id <uuid>             Existing hosting_nodes ID (recommended)
  --secret <secret>            Node secret (at least 32 random characters)
  --name <name>                Node display name
  --region <region>            Node region
  --host <host>                Node public host/IP
  --data-dir <dir>             Data directory
  --java <path>                Path to Java executable
  --heartbeat-interval <s>     Heartbeat interval (default: 30)
  --poll-interval <s>          Operation poll interval (default: 5)
  --telemetry-interval <s>     Telemetry interval (default: 60)
  -h, --help                   Show this help

ENVIRONMENT VARIABLES:
  SUPABASE_URL, SUPABASE_ANON_KEY, SUPABASE_ACCESS_TOKEN, SUPABASE_REFRESH_TOKEN
  AMALGAM_NODE_ID, AMALGAM_NODE_SECRET, AMALGAM_NODE_NAME
  AMALGAM_REGION, AMALGAM_HOST, AMALGAM_DATA_DIR, AMALGAM_JAVA_PATH

SECURE SETUP:
  1. An authenticated launcher/API user provisions a hosting_nodes row.
  2. Store the returned node ID and one-time secret in this agent's config.
  3. Run with the anon key and an owner access token, or a narrowly scoped
     service integration that can call node-scoped RPCs for the provisioned ID.
  The agent never auto-creates a node with a service-role key.
`);
}

export function ensureDataDir(config) {
  if (!fs.existsSync(config.dataDir)) fs.mkdirSync(config.dataDir, { recursive: true });
}
