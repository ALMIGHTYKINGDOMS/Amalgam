const state = {
  version: "3.0.0-beta.3",
  channel: "beta",
  loaded: false,
  backend: "offline",
  settings: {
    enabled: true,
    hudEnabled: true,
    notificationsEnabled: true,
    hudPreset: "top_left",
    hudScale: 1,
    hudOpacity: 0.92,
    language: "en_us"
  },
  session: {
    worldName: "",
    dimension: "",
    playerName: "",
    startedAt: 0,
    serverName: "",
    serverAddress: ""
  },
  menuOpen: false
};

export function getState() { return state; }
export function setLoaded(value) { state.loaded = Boolean(value); }
export function updateSettings(patch) { Object.assign(state.settings, patch); }
export function updateSession(patch) { Object.assign(state.session, patch); }
