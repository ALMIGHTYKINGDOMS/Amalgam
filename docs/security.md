# Security And Privacy

## Secrets

- `launcher.json` lives beside the launcher executable and is local-only.
- AI keys, the Modrinth token, and the CurseForge key are configuration values, never source code or bridge payloads. When saved, they are protected with Windows DPAPI for the current Windows account; older plaintext local settings migrate on their next successful save.
- Published Settings does not expose those credentials. The local Admin control
  center requires an 8-character minimum password, stores only a salted PBKDF2
  verifier, applies a session timeout and failed-attempt backoff, and masks all
  credential inputs. The password itself is never written to `launcher.json`.
- If a protected setting belongs to another Windows account, Amalgam refuses to overwrite it silently. The Settings screen explains how to re-enter or explicitly forget that inaccessible credential.
- Microsoft account tokens obtained by `--login` are stored separately under `%LOCALAPPDATA%\Amalgam` and protected with Windows DPAPI; they are not written to `launcher.json`.
- `launcher.json`, logs, heap dumps, crash dumps, build output, and IDE files are ignored by `.gitignore`.
- Rotate any credential that has been pasted into chat, logs, screenshots, or a crash report. The launcher cannot rotate provider credentials for you.

## Network

All launcher downloads and API requests pass through `net.cpp`:

- only `https` URLs are accepted;
- URLs containing embedded user credentials are rejected;
- connect timeout is 30 seconds and receive/send timeout is 60 seconds;
- requests retry up to three times where the operation is safe to retry;
- JSON responses are capped at 64 MiB and downloads at 2 GiB;
- downloads are written to a `.part` file, resumed with an HTTP byte range when
  the server supports it, and renamed only after completion;
- Mojang version metadata, libraries, client jars, asset indexes, and asset objects are checked against metadata SHA-1 and size before reuse; auto-provisioned Adoptium JDK archives use the published SHA-256 and size.

## Archive handling

Installer and native archives are checked before extraction. The extractor rejects invalid zip signatures, absolute paths, traversal (`..`), drive-qualified paths, alternate data streams, wildcards, control characters, oversized archives, excessive entry counts, and reparse-point escapes. Extraction happens in a staging directory before files are moved into the instance.

## Java bridge and telemetry

- Actions remain limited to the existing 36-byte action protocol and are drained at `START_CLIENT_TICK` / the equivalent NeoForge pre-tick event; pending actions are cleared when the player/world disappears or safe mode activates.
- The game-mode channel is a separate bounded `TLM1` JNI payload. It contains only the visible sidebar title and at most 15 sanitized lines. It is not a Minecraft network packet and never enters the action queue.
- The native parser strips formatting/control characters, bounds all strings, and clears state when the player or sidebar disappears.
- Current game-mode features are read-only HUD/tracking: scoreboard parsing, mode detection, timers, team/status facts, and alerts. They do not send custom packets or automate competitive actions.

## Diagnostics and future beta

Sidebar telemetry is used locally for the HUD. Match summaries and manual markers can be exported locally; persistence is disabled by default. If the user explicitly enables local diagnostics, Amalgam uses the Windows 10+ `winsqlite3` component under `%LOCALAPPDATA%\Amalgam` with:

- explicit opt-in and a bounded local outbox;
- aggregate numeric match summaries only;
- export and delete controls;
- no credentials, tokens, server addresses, world files, screenshots, chat, coordinates, player names, inventory, or mod jars;
- no network upload in the current implementation. A future HTTPS beta contract is documented in `docs/diagnostics-api.md` and must use a short-lived service token, not a client-shipped secret.

## Threat model limits

Amalgam is a personal utility client and launcher, not a sandbox or anti-cheat product. Do not use it to evade detection, bypass access controls, hide automation, or obtain an unfair advantage on servers. Keep server-safe mode enabled when using telemetry-only features on test servers.
