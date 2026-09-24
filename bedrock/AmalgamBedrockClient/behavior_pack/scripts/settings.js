import { DEFAULT_SETTINGS, getPlayerSettings, replacePlayerSettings, updatePlayerSettings } from "./util/state.js";
import { loadSettings, saveSettings, resetSettings } from "./storage.js";
import { info } from "./util/logger.js";

export function loadClientSettings(player) {
  replacePlayerSettings(player, loadSettings(player, getPlayerSettings(player)));
  return getPlayerSettings(player);
}

export function setSetting(player, key, value) {
  const allowed = new Set(["enabled", "hudEnabled", "notificationsEnabled", "language"]);
  if (!allowed.has(key)) return false;
  updatePlayerSettings(player, { [key]: value });
  saveSettings(player, getPlayerSettings(player));
  info("settings", `${key} updated for ${player?.name ?? "player"}`);
  return true;
}

export function resetClientSettings(player) {
  resetSettings(player);
  replacePlayerSettings(player, DEFAULT_SETTINGS);
}
