# Amalgam Architecture

Amalgam is a Minecraft launcher for Windows built with C++17 and Dear ImGui.

## Directory layout

- `cpp/launcher/` — the desktop launcher (Windows app)
  - `src/` — launcher source
  - `ui.cpp` — main ImGui UI, one function per tab
  - `launch.cpp` — Minecraft launch pipeline (versions, libraries, assets, natives)
  - `java.cpp` — managed Java runtime (Adoptium API download, validation)
  - `server_manager.cpp` — local server lifecycle
  - `instances.cpp` — profiles/instances
  - `mods.cpp` — Modrinth/CurseForge content APIs
- `cpp/ai/` — built-in Amalgam AI module
- `bedrock/AmalgamBedrockClient/` — Bedrock add-on
- `supabase/` — backend Edge Functions and migrations
- `installer/` — Inno Setup installer script

## Launcher profile model

Profiles live in `instances/<profile-id>/` and contain:
- `minecraft/` — the actual game files (versions, libraries, assets)
- `mods/` — installed mod JARs
- `config/` — mod configs
- `kubejs/` — KubeJS scripts (if KubeJS installed)
- `datapacks/` — datapacks
- `resourcepacks/` — resource packs
- `saves/` — worlds

## AI Profile model

An AI Profile is a normal profile plus:
- `ai-conversation.json` — persistent per-profile conversation
- `ai-checkpoints/` — checkpoint snapshots for undo
- `ai-plans/` — structured creation plans

## Versions

- Launcher version: 1.0.0
- Channel: stable
- Minecraft versions: any version the launcher supports
- Loaders: Vanilla, Fabric, Forge, NeoForge, Quilt (experimental)
