import { getState } from "./util/state.js";
import { info } from "./util/logger.js";

// Bedrock Script API does not expose a trustworthy remote server address or a
// client-side online token. Keep the add-on transport-free: local features
// work in every world, while account/social/server metadata stays launcher-
// owned and is never guessed here.
export function connectionCapabilities() {
  const state = getState();
  return {
    localFeatures: true,
    multiplayerSafe: true,
    backendConnected: state.backend === "connected",
    serverMetadata: Boolean(state.session.serverName || state.session.serverAddress),
    message: "Local Amalgam features work offline and during supported multiplayer sessions."
  };
}

export function markLauncherConnection(status) {
  const state = getState();
  state.backend = status === "connected" ? "connected" : "offline";
  info("connection", `launcher backend ${state.backend}`);
}
