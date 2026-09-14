import { world } from "@minecraft/server";
import { getState, setLoaded } from "./util/state.js";
import { loadClientSettings } from "./settings.js";
import { info, error } from "./util/logger.js";

export function bootstrap() {
  try {
    loadClientSettings();
    setLoaded(true);
    info("bootstrap", `loaded ${getState().version}`);
    return true;
  } catch (e) {
    error("bootstrap", `load failed: ${e}`);
    return false;
  }
}

export function worldReady() {
  try { return world.getAllPlayers().length > 0; } catch { return false; }
}
