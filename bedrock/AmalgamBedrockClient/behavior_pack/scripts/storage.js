import { warn } from "./util/logger.js";

const KEY = "amalgam:settings-v2";

function normalizedSettings(settings, fallback) {
  const source = { ...fallback, ...settings };
  return {
    enabled: Boolean(source.enabled),
    hudEnabled: Boolean(source.hudEnabled),
    notificationsEnabled: Boolean(source.notificationsEnabled),
    language: String(source.language ?? "en_us").slice(0, 16)
  };
}

export function loadSettings(player, fallback) {
  try {
    const raw = player?.getDynamicProperty(KEY);
    if (typeof raw !== "string" || !raw) return { ...fallback };
    const parsed = JSON.parse(raw);
    return normalizedSettings(parsed, fallback);
  } catch (e) {
    warn("storage", `settings load skipped: ${e}`);
    return { ...fallback };
  }
}

export function saveSettings(player, settings) {
  try {
    if (!player) return false;
    player.setDynamicProperty(KEY, JSON.stringify(normalizedSettings(settings, {})));
    return true;
  } catch (e) {
    warn("storage", `settings save skipped: ${e}`);
    return false;
  }
}

export function resetSettings(player) {
  try { player?.setDynamicProperty(KEY, undefined); } catch { /* optional */ }
}
