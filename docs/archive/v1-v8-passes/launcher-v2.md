# Launcher V2 Scope

Amalgam V2 keeps the existing C++/ImGui launcher and upgrades its workflows around isolated profiles. The implementation uses Prism/MultiMC-style instance separation, Packwiz/.mrpack-style manifest handling, and Ferium-style compatibility/hash checks as design references. No external GPL/MPL source was copied.

## Implemented

- **Library:** profile cards, search, favorites, groups, recent/name/favorite sorting, prepared-directory discovery, custom profile creation, import, launch, duplicate, export manifest, repair/validate, folder access, and delete confirmation.
- **Profile page:** Overview, Installed Content, Worlds, Screenshots, Versions, Logs, Settings, and Manage tabs.
- **Performance:** Each profile can select Auto, Low-end, Balanced, Shaders, Heavy modpack, or Custom tuning. Explicit profiles can apply reversible `options.txt` presets and install compatible performance mods from Modrinth.
- **Profile wizard:** Source cards, creator project IDs/slugs, archive import, AI planning, version/loader selection, performance tuning, Java/memory settings, review, and isolated creation are handled in one four-step flow.
- **Local content:** Installed Content has an `Upload local` action for `.jar` mods and `.zip` resource packs, shader packs, datapacks, or config archives. Imported files are collision-safe, SHA-1 recorded in `amalgam-local-content.json`, and included by profile exports.
- **Bedrock addons:** The Bedrock page accepts `.mcpack` and `.mcaddon` files from CurseForge or any other source through a file picker and passes them through the existing UWP import flow.
- **Installed Content:** Mods, resource packs, shaders, data packs, and config files are enumerated per profile. Files can be enabled/disabled by safe rename or removed without affecting other profiles.
- **Profile-scoped Add Content:** the selected profile’s Minecraft version and loader are carried into the Modrinth/CurseForge browser and recursive dependency installer.
- **Modpack import:** Modrinth `.mrpack` and CurseForge archives, overrides, client compatibility filtering, and SHA-1/size verification.
- **Project install:** Modpack cards install the creator's latest compatible archive as a new isolated profile from its manifest; individual mod cards require an existing selected profile and compatible version/loader.
- **Modpack export:** profile overrides export to valid Modrinth `.mrpack` and CurseForge-style ZIP archives, plus an Amalgam metadata manifest.
- **Updates/recovery:** ownership metadata is persisted for new installs; Update All verifies compatible replacements and creates a restore point first. Published creator-pack updates recheck the compatible upstream archive, show its release/changelog, stage it before commit, and create an updated copy instead of touching a locally modified profile. Latest restore points can be restored from Manage.
- **Recovery-first library actions:** removing profile content, screenshots, worlds, or a whole profile moves it into a profile/library recovery location rather than permanently deleting it. Worlds also have a separate backup action outside Minecraft's `saves` folder.
- **Published/custom state:** imported packs retain source/project/version metadata and become visibly `Published` or `Modified`; optional dependencies are recorded separately without being silently installed.
- **Downloads:** job history persists in `downloads.json`; interrupted jobs are marked failed on restart and import progress honors cancellation between files.
- **Forge 1.20.1:** dedicated client bridge with Insert-key in-game panel, module toggles, parameters, server-safe mode, telemetry, AutoTool, and Freecam camera mixin.
- **Global downloads:** existing job queue/panel remains available while navigating other pages.
- **Java/runtime:** per-profile Java/memory settings, cached runtime reuse, prerequisite checks, and Forge 1.20.1 Java 17 selection.

## Still Deliberately Partial

- Published-pack updates show the compatible release, archive details, creator changelog, confirmation/recovery plan, and stage the archive before commit. A full downloaded-archive per-file added/removed/replaced diff is not yet rendered before confirmation.
- Older manually copied JARs have no provider ownership metadata; new managed installs persist project/source/version/required-by records.
- Crash classification now reports common Java/mod/mixin/OpenAL/memory failure classes, but it is not yet a full dependency-conflict analyzer.
- Forge 1.12.2, 1.18.2, 1.19.2, and 1.20.1 now have bridges. Bedrock remains a separate UWP adapter.
