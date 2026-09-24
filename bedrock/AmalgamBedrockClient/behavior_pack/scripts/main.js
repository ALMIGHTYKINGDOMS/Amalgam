import { world, system } from "@minecraft/server";
import { bootstrap } from "./bootstrap.js";
import { openMenu } from "./menu.js";
import { renderHud } from "./hud.js";
import { readPlayerState } from "./player.js";
import { startSession } from "./session.js";
import { loadClientSettings } from "./settings.js";
import { notify } from "./notifications.js";
import { info } from "./util/logger.js";
import { clearPlayerState, isPlayerMenuOpen } from "./util/state.js";

// Keep the world adapter private to the pack; hud.js uses it only for its loop.
globalThis.__amalgamWorld = world;
bootstrap();

world.afterEvents.playerSpawn.subscribe((event) => {
  const player = event.player;
  loadClientSettings(player);
  readPlayerState(player);
  if (event.initialSpawn) {
    startSession(player);
    notify(player, "Amalgam Bedrock Client", "Loaded in supported stable mode. Use /scriptevent amalgam:menu for the client menu.");
  }
  renderHud(player);
});

system.afterEvents.scriptEventReceive.subscribe((event) => {
  if (event.id !== "amalgam:menu") return;
  openMenu(event.sourceEntity).catch((error) => info("menu", `open failed: ${error}`));
});

// Online-safe fallback trigger. This does not grant permissions or issue
// gameplay commands: it only opens the local client menu when the player is
// sneaking and uses a compass. It works in single-player and multiplayer when
// the behavior pack is enabled for the current world/server.
world.afterEvents.itemUse.subscribe((event) => {
  const player = event.source;
  const item = event.itemStack;
  if (!player || item?.typeId !== "minecraft:compass" || !player.isSneaking || isPlayerMenuOpen(player)) return;
  openMenu(player).catch((error) => info("menu", `compass trigger failed: ${error}`));
});

system.runInterval(() => {
  for (const player of world.getAllPlayers()) {
    readPlayerState(player);
    renderHud(player);
  }
}, 10);

world.afterEvents.playerLeave.subscribe((event) => {
  clearPlayerState(event.playerId);
});
