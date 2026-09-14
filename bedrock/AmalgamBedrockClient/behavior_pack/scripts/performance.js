import { getState } from "./util/state.js";

export function supportedPerformanceSummary() {
  return {
    profile: "supported",
    notes: "Bedrock performance controls remain owned by Minecraft settings. Amalgam does not fabricate FPS, TPS, ping, CPU, or RAM values.",
    clientEnabled: Boolean(getState().settings.enabled)
  };
}
