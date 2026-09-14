import { ActionFormData, ModalFormData } from "@minecraft/server-ui";
import { getState } from "./util/state.js";
import { setSetting, resetClientSettings } from "./settings.js";
import { diagnosticsText } from "./diagnostics.js";
import { notify } from "./notifications.js";
import { connectionCapabilities } from "./connection.js";

export async function openMenu(player) {
  if (!player) return;
  const state = getState();
  state.menuOpen = true;
  try {
    const result = await new ActionFormData()
      .title("Amalgam Bedrock Client")
      .body(`Beta ${state.version}\n${connectionCapabilities().message}\nNo online data is fabricated.`)
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
    notify(player, "Amalgam Bedrock Client", "Supported Bedrock companion — beta.");
  } finally {
    state.menuOpen = false;
  }
}

async function openHudSettings(player) {
  const s = getState().settings;
  const result = await new ModalFormData()
    .title("HUD")
    .toggle("Show HUD", s.hudEnabled)
    .dropdown("Preset", ["Top left", "Top right", "Bottom left", "Bottom right", "Center top"], ["top_left", "top_right", "bottom_left", "bottom_right", "center_top"].indexOf(s.hudPreset))
    .slider("Scale", 0.75, 1.5, 0.05, s.hudScale)
    .slider("Opacity", 0.2, 1, 0.05, s.hudOpacity)
    .show(player);
  if (result.canceled || !result.formValues) return;
  const presets = ["top_left", "top_right", "bottom_left", "bottom_right", "center_top"];
  setSetting("hudEnabled", Boolean(result.formValues[0]));
  setSetting("hudPreset", presets[Number(result.formValues[1])] || "top_left");
  setSetting("hudScale", Number(result.formValues[2]));
  setSetting("hudOpacity", Number(result.formValues[3]));
  notify(player, "HUD updated", "Your supported HUD settings were saved.");
}

async function openSettings(player) {
  const s = getState().settings;
  const result = await new ActionFormData()
    .title("Settings")
    .body("Only local, supported settings are changed in-game.")
    .button(`HUD: ${s.hudEnabled ? "On" : "Off"}`)
    .button(`Notifications: ${s.notificationsEnabled ? "On" : "Off"}`)
    .button(`Client: ${s.enabled ? "Enabled" : "Disabled"}`)
    .button("Reset local settings")
    .show(player);
  if (result.canceled) return;
  if (result.selection === 0) setSetting("hudEnabled", !s.hudEnabled);
  if (result.selection === 1) setSetting("notificationsEnabled", !s.notificationsEnabled);
  if (result.selection === 2) setSetting("enabled", !s.enabled);
  if (result.selection === 3) resetClientSettings();
  notify(player, "Settings saved", "Changes apply locally to this world.");
}
