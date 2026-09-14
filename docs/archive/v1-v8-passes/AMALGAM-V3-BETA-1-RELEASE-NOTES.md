# Amalgam V3 — Beta 1 Release Notes

Version: **3.0.0-beta.1** · Channel: **beta** · Stage: Closed Beta (Wave 0/1)

## What's New

- **New V3 launcher shell** — Home, Discover, Library, Downloads, Profiles,
  Essentials, Local Servers, Bedrock, Settings, Account, and Diagnostics.
- **Java Runtime Manager** — auto-download and manage Java 8/11/17/21/25.
- **Loaders** — Vanilla, Fabric, Forge (1.12.2–1.20.1), NeoForge (1.21.x),
  and Quilt (experimental).
- **Discover** — real Modrinth and CurseForge search, details, dependencies,
  and install into a profile.
- **Local Servers** — create, install, start, console, stop, backups for
  Paper, Purpur, Folia, Fabric, Quilt, Vanilla (manual), Spigot (manual).
- **In-game client** — Insert-key menu, HUD with editor, performance
  graphs, settings that persist.
- **Essentials (beta)** — friends and sessions with direct peer-to-peer;
  relay (TURN) is being validated.
- **Atomic configuration saves** — launcher settings survive crashes with a
  last-known-good backup.
- **Self-update** — signed, staged updates with anti-rollback protection and
  automatic rollback if a replacement fails.
- **Beta tooling** — Known Issues, Report a Bug, explicit safe mode, and crash
  recovery tools without interrupting normal startup.

## What To Test

1. Fresh install, then create a Vanilla profile and launch Minecraft.
2. Create a Fabric profile, install a mod from Discover, launch.
3. Forge and NeoForge profiles on supported versions.
4. Local Server: create a Paper server, start it, read the console, stop it.
5. Launch a profile with a friend over Essentials direct connection.
6. Settings: change something, restart the launcher, confirm it persists.
7. Update check: open Settings → About → Check for Updates.
8. Confirm the launcher opens directly to the main workspace without a beta or
   recovery popup. If troubleshooting is needed, relaunch it explicitly with
   `--safe-mode`.

## Known Issues

- Essentials relay (TURN) is beta and may be unavailable; direct P2P still
  works. The UI shows the real connection state.
- Quilt loader support is experimental. Use Fabric or Vanilla if a Quilt
  profile misbehaves.
- Amalgam Cloud hosting is **Coming Soon** — purchase and deploy are
  disabled in this build.
- Bedrock features require Minecraft for Windows to be installed.
- Some account features are still being finalized and may change between
  beta builds.

## Experimental Features

- Quilt loader (client + server).
- Essentials relay connections.

## How To Report Bugs

Use **Settings → Report a Bug** (or the Help area). Include:

- What you were doing
- Expected vs actual behavior
- The launcher version (v3.0.0-beta.1)
- Your Windows version and Minecraft version/loader

Logs live in `%LOCALAPPDATA%\AmalgamLauncher\logs` and
`<install dir>\logs`. Do **not** paste tokens, passwords, or API keys into
a bug report.
