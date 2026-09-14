const listeners = new Map();

export function on(name, listener) {
  if (!listeners.has(name)) listeners.set(name, new Set());
  listeners.get(name).add(listener);
  return () => listeners.get(name)?.delete(listener);
}

export function emit(name, payload) {
  for (const listener of listeners.get(name) ?? []) {
    try { listener(payload); } catch { /* one listener must not break the client */ }
  }
}
