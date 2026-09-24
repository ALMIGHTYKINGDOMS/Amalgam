import { system } from "@minecraft/server";
import { getPlayerSettings } from "./util/state.js";
import { sessionSummary } from "./session.js";

function formatLocation(location) {
  return `${Math.floor(location.x)}, ${Math.floor(location.y)}, ${Math.floor(location.z)}`;
}

export function renderHud(player) {
  const settings = getPlayerSettings(player);
  if (!settings.enabled || !settings.hudEnabled || !player) return;
  const summary = sessionSummary(player);
  const location = formatLocation(player.location);
  const dimension = summary.dimension.replace("minecraft:", "");
  const text = `§dAMALGAM §8• §f${summary.worldName}\n§7${dimension} §8• §7${location} §8• §aLocal client`;
  try { player.onScreenDisplay.setActionBar(text); } catch { /* actionbar is optional on older clients */ }
}

export function startHudLoop() {
  return system.runInterval(() => {
    for (const player of worldPlayers()) renderHud(player);
  }, 10);
}

function worldPlayers() {
  try {
    // Kept behind a small adapter so the loop remains easy to update if the API moves.
    return globalThis.__amalgamWorld?.getAllPlayers?.() ?? [];
  } catch { return []; }
}
