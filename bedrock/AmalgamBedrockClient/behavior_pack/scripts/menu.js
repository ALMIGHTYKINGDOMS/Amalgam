import { ActionFormData } from "@minecraft/server-ui";
import { getPlayerSettings, getState, isPlayerMenuOpen, setPlayerMenuOpen } from "./util/state.js";
import { setSetting, resetClientSettings } from "./settings.js";
import { diagnosticsText } from "./diagnostics.js";
import { notify } from "./notifications.js";
import { connectionCapabilities } from "./connection.js";

export async function openMenu(player) {
  if (!player || isPlayerMenuOpen(player)) return;
  const state = getState();
  setPlayerMenuOpen(player, true);
  try {
    const result = await new ActionFormData()
      .title("Amalgam Bedrock Client")
      .body(`${state.channel === "stable" ? "Stable" : `${state.channel} channel`} ${state.version}\n${connectionCapabilities(player).message}\nNo online data is fabricated.`)
      .button("HUD", "textures/ui/amalgam_logo")
      .button("Settings", "textures/ui/amalgam_logo")
      .button("Diagnostics", "textures/ui/amalgam_logo")
      .button("Social", "textures/ui/amalgam_logo")
      .button("Servers", "textures/ui/amalgam_logo")
      .button("About", "textures/ui/amalgam_logo")
      .show(player);
    if (result.canceled) return;
    if (result.selection === 0) return openHudSettings(player);
    if (result.selection === 1) return openSettings(player);
    if (result.selection === 2) {
      player.sendMessage(`§d[Amalgam diagnostics]§r\n${diagnosticsText(player)}`);
      return;
    }
    if (result.selection === 3 || result.selection === 4) {
      notify(player, result.selection === 3 ? "Social" : "Servers", "Online details are managed by the launcher.");
      return;
    }
    notify(player, "Amalgam Bedrock Client", `Supported Bedrock companion — ${state.channel} ${state.version}.`);
  } finally {
    setPlayerMenuOpen(player, false);
  }
}

async function openHudSettings(player) {
  const settings = getPlayerSettings(player);
  const result = await new ActionFormData()
    .title("Action-bar HUD")
    .body("Amalgam uses Minecraft's action bar. Minecraft controls its position, size, and opacity, so this pack can only show or hide the HUD.")
    .button(`HUD: ${settings.hudEnabled ? "On" : "Off"}`)
    .show(player);
  if (result.canceled || result.selection !== 0) return;
  setSetting(player, "hudEnabled", !settings.hudEnabled);
  notify(player, "HUD updated", "Only the action-bar HUD visibility is configurable in Bedrock.");
}

async function openSettings(player) {
  const settings = getPlayerSettings(player);
  const result = await new ActionFormData()
    .title("Settings")
    .body("Only supported settings for your player are changed in-game. They are not shared with other players.")
    .button(`HUD: ${settings.hudEnabled ? "On" : "Off"}`)
    .button(`Notifications: ${settings.notificationsEnabled ? "On" : "Off"}`)
    .button(`Client: ${settings.enabled ? "Enabled" : "Disabled"}`)
    .button("Reset my settings")
    .show(player);
  if (result.canceled) return;
  if (result.selection === 0) setSetting(player, "hudEnabled", !settings.hudEnabled);
  if (result.selection === 1) setSetting(player, "notificationsEnabled", !settings.notificationsEnabled);
  if (result.selection === 2) setSetting(player, "enabled", !settings.enabled);
  if (result.selection === 3) resetClientSettings(player);
  notify(player, "Settings saved", "Changes apply only to your player in this Bedrock world.");
}
