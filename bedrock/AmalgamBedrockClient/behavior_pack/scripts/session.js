import { getState, updateSession } from "./util/state.js";

export function startSession(player) {
  updateSession({ startedAt: Date.now(), playerName: player?.name ?? "Player" });
}

export function sessionSummary() {
  const session = getState().session;
  return {
    worldName: session.worldName || "Current World",
    dimension: session.dimension || "unknown",
    playerName: session.playerName || "Player",
    serverName: session.serverName || "",
    elapsedMs: session.startedAt ? Math.max(0, Date.now() - session.startedAt) : 0
  };
}
