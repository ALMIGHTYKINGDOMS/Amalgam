import { getState } from "./util/state.js";

export function notify(player, title, body = "") {
  if (!getState().settings.notificationsEnabled || !player) return;
  const safeTitle = String(title).slice(0, 80);
  const safeBody = String(body).slice(0, 180);
  try {
    player.onScreenDisplay.setTitle(safeTitle, { subtitle: safeBody, stayDuration: 40, fadeInDuration: 5, fadeOutDuration: 10 });
  } catch {
    try { player.sendMessage(`§d[Amalgam]§r ${safeTitle}${safeBody ? ` — ${safeBody}` : ""}`); } catch { /* best effort */ }
  }
}
