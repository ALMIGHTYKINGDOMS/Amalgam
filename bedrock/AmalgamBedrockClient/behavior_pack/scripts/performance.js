import { getPlayerSettings } from "./util/state.js";

export function supportedPerformanceSummary(player) {
  return {
    profile: "supported",
    notes: "Bedrock performance controls remain owned by Minecraft settings. Amalgam does not fabricate FPS, TPS, ping, CPU, or RAM values.",
    clientEnabled: Boolean(getPlayerSettings(player).enabled)
  };
}
