import { getPlayerSession, updatePlayerSession } from "./util/state.js";

export function startSession(player) {
  updatePlayerSession(player, { startedAt: Date.now(), playerName: player?.name ?? "Player" });
}

export function sessionSummary(player) {
  const session = getPlayerSession(player);
  return {
    worldName: session.worldName || "Current World",
    dimension: session.dimension || "unknown",
    playerName: session.playerName || "Player",
    serverName: session.serverName || "",
    elapsedMs: session.startedAt ? Math.max(0, Date.now() - session.startedAt) : 0
  };
}
