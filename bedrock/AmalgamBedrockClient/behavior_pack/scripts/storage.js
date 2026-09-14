import { world } from "@minecraft/server";
import { warn } from "./util/logger.js";

const KEY = "amalgam:settings";

export function loadSettings(fallback) {
  try {
    const raw = world.getDynamicProperty(KEY);
    if (typeof raw !== "string" || !raw) return { ...fallback };
    const parsed = JSON.parse(raw);
    return { ...fallback, ...parsed };
  } catch (e) {
    warn("storage", `settings load skipped: ${e}`);
    return { ...fallback };
  }
}

export function saveSettings(settings) {
  try {
    world.setDynamicProperty(KEY, JSON.stringify({
      enabled: Boolean(settings.enabled),
      hudEnabled: Boolean(settings.hudEnabled),
      notificationsEnabled: Boolean(settings.notificationsEnabled),
      hudPreset: String(settings.hudPreset).slice(0, 24),
      hudScale: Math.max(0.75, Math.min(1.5, Number(settings.hudScale) || 1)),
      hudOpacity: Math.max(0.2, Math.min(1, Number(settings.hudOpacity) || 0.92)),
      language: String(settings.language).slice(0, 16)
    }));
    return true;
  } catch (e) {
    warn("storage", `settings save skipped: ${e}`);
    return false;
  }
}

export function resetSettings() {
  try { world.setDynamicProperty(KEY, undefined); } catch { /* optional */ }
}
