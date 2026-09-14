import { getState, updateSettings } from "./util/state.js";
import { loadSettings, saveSettings, resetSettings } from "./storage.js";
import { info } from "./util/logger.js";

export function loadClientSettings() {
  updateSettings(loadSettings(getState().settings));
}

export function setSetting(key, value) {
  const allowed = new Set(["enabled", "hudEnabled", "notificationsEnabled", "hudPreset", "hudScale", "hudOpacity", "language"]);
  if (!allowed.has(key)) return false;
  updateSettings({ [key]: value });
  saveSettings(getState().settings);
  info("settings", `${key} updated`);
  return true;
}

export function resetClientSettings() {
  resetSettings();
  const defaults = { enabled: true, hudEnabled: true, notificationsEnabled: true, hudPreset: "top_left", hudScale: 1, hudOpacity: 0.92, language: "en_us" };
  updateSettings(defaults);
  saveSettings(getState().settings);
}
