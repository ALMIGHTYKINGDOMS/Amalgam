import { getPlayerSession, getPlayerSettings, getState } from "./util/state.js";
import { recentLogs } from "./util/logger.js";
import { supportedPerformanceSummary } from "./performance.js";
import { connectionCapabilities } from "./connection.js";

export function collectDiagnostics(player) {
  const state = getState();
  const settings = getPlayerSettings(player);
  const session = getPlayerSession(player);
  return {
    client: { name: "Amalgam Bedrock Client", version: state.version, channel: state.channel, enabled: settings.enabled },
    minecraft: { player: player?.name ?? "", dimension: player?.dimension?.id ?? "unknown", world: session.worldName || "Current World" },
    packs: { behavior: "loaded", resource: "loaded", script: state.loaded ? "loaded" : "starting" },
    connection: { backend: state.backend, server: session.serverName || "", capabilities: connectionCapabilities(player) },
    performance: supportedPerformanceSummary(player),
    logs: recentLogs()
  };
}

export function diagnosticsText(player) {
  const report = collectDiagnostics(player);
  return JSON.stringify(report, null, 2).slice(0, 6000);
}
