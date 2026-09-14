import { updateSession } from "./util/state.js";

export function readPlayerState(player) {
  const dimension = player?.dimension?.id ?? "unknown";
  const worldName = player?.getTags?.().find((tag) => tag.startsWith("amalgam:world="))?.slice(15) || "Current World";
  updateSession({
    playerName: player?.name ?? "Player",
    dimension,
    worldName,
    serverName: "",
    serverAddress: ""
  });
  return { playerName: player?.name ?? "Player", dimension, worldName };
}
