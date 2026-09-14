const MAX_ENTRIES = 80;
const entries = [];

function now() {
  return new Date().toISOString();
}

function clean(value) {
  return String(value ?? "").replace(/[\r\n]+/g, " ").slice(0, 240);
}

export function log(level, component, message) {
  entries.push({ timestamp: now(), level, component: clean(component), message: clean(message) });
  while (entries.length > MAX_ENTRIES) entries.shift();
}

export function info(component, message) { log("info", component, message); }
export function warn(component, message) { log("warning", component, message); }
export function error(component, message) { log("error", component, message); }
export function recentLogs() { return entries.slice(); }

