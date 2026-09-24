import { world } from "@minecraft/server";
import { getState, setLoaded } from "./util/state.js";
import { info, error } from "./util/logger.js";

export function bootstrap() {
  try {
    setLoaded(true);
    info("bootstrap", `loaded ${getState().version} (${getState().channel})`);
    return true;
  } catch (e) {
    error("bootstrap", `load failed: ${e}`);
    return false;
  }
}

export function worldReady() {
  try { return world.getAllPlayers().length > 0; } catch { return false; }
}
