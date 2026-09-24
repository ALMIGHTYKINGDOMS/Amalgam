import { RUNTIME_METADATA } from "./runtime_metadata.js";

export const DEFAULT_SETTINGS = Object.freeze({
  enabled: true,
  hudEnabled: true,
  notificationsEnabled: true,
  language: "en_us"
});

function defaultSession() {
  return {
    worldName: "",
    dimension: "",
    playerName: "",
    startedAt: 0,
    serverName: "",
    serverAddress: ""
  };
}

const state = {
  version: RUNTIME_METADATA.version,
  channel: RUNTIME_METADATA.channel,
  loaded: false,
  backend: "offline"
};
const playerStates = new Map();

function playerKey(player) {
  const id = typeof player?.id === "string" ? player.id : "";
  return id ? `player:${id}` : "";
}

function createPlayerState() {
  return {
    settings: { ...DEFAULT_SETTINGS },
    session: defaultSession(),
    menuOpen: false
  };
}

function ensurePlayerState(player) {
  const key = playerKey(player);
  if (!key) return createPlayerState();
  if (!playerStates.has(key)) playerStates.set(key, createPlayerState());
  return playerStates.get(key);
}

export function getState() { return state; }
export function setLoaded(value) { state.loaded = Boolean(value); }

export function getPlayerState(player) { return ensurePlayerState(player); }
export function getPlayerSettings(player) { return ensurePlayerState(player).settings; }
export function getPlayerSession(player) { return ensurePlayerState(player).session; }

export function replacePlayerSettings(player, settings) {
  ensurePlayerState(player).settings = { ...DEFAULT_SETTINGS, ...settings };
}

export function updatePlayerSettings(player, patch) {
  Object.assign(ensurePlayerState(player).settings, patch);
}

export function updatePlayerSession(player, patch) {
  Object.assign(ensurePlayerState(player).session, patch);
}

export function isPlayerMenuOpen(player) { return ensurePlayerState(player).menuOpen; }
export function setPlayerMenuOpen(player, value) { ensurePlayerState(player).menuOpen = Boolean(value); }

export function clearPlayerState(playerOrId) {
  const key = typeof playerOrId === "string" ? `player:${playerOrId}` : playerKey(playerOrId);
  if (key) playerStates.delete(key);
}
