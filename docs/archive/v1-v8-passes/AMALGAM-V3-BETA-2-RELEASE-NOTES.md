# Amalgam V3 Beta 2 — Release Notes

Version: **3.0.0-beta.2** · Channel: **beta** · Stage: Closed Beta

## What changed

- Refined the Minecraft-themed launcher shell, catalog cards, project pages, and responsive layouts.
- Added clearer Microsoft device-code and Minecraft/Xbox approval messages, including the official-launcher fallback.
- Improved catalog artwork fallbacks and bounded provider icon downloads to keep the UI responsive.
- Added keyboard navigation shortcuts when enabled in Settings → Theme & Accessibility.
- Added the complete launcher and Bedrock branding/art set to the release package.
- Added self-contained license, installation information, tester guide, integrity manifests, and SBOM files.

## Beta limitations

- Microsoft direct sign-in requires the registered application to be approved for Minecraft/Xbox services. Until then, use the official Minecraft Launcher sign-in path.
- CurseForge access requires a valid personal API key or an authenticated Amalgam provider service. Modrinth is public and does not require a token.
- Quilt support is experimental.
- Bedrock support requires Minecraft for Windows and uses the supported add-on model; it is not the Java DLL injection path.
- Amalgam Cloud hosting is not enabled in this beta.
- The launcher, DLL, installer, and updater payloads must be code-signed before public distribution.

## Tester expectations

- Back up profiles and worlds before testing installs, updates, or loader changes.
- Report the exact page, action, Minecraft version, loader, and provider used.
- Attach launcher logs and crash reports when a launch or installation fails.
