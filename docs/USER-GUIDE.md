# Amalgam Launcher User Guide

## Before you begin

- Use a Windows 10 or newer x64 PC.
- Own the edition of Minecraft you intend to play.
- Keep the official Minecraft Launcher installed for Java Edition. Bedrock is
  currently labelled **Coming Soon**; its runtime actions are intentionally
  disabled in this release while the supported Windows integration is gated.
- Back up worlds you care about before changing or importing a modpack.

## First launch

1. Open **Amalgam Launcher**.
2. Create a profile from **Library**. The profile wizard only lists Minecraft
   versions and loader combinations carried by this release, so you do not
   need to guess a version number.
3. Use **Discover** to browse content from the configured providers. Every
   item opens a project page before installation.
4. Install a modpack into its own profile. Amalgam keeps every profile in a
   separate folder so one pack does not overwrite another.

## Minecraft accounts

Amalgam never asks for your Microsoft password. Direct Amalgam device-code
sign-in is enabled only after the publisher's Minecraft application approval
is active. Until then, the launcher directs you to use the official Minecraft
Launcher for account sign-in and ownership checks.

## Java, Bedrock, and content

- Java profiles use a supported Fabric, Quilt, Forge, NeoForge, or Vanilla
  target selected in the profile wizard.
- Bedrock is a **Coming Soon** surface in 1.0.0. The release package includes
  statically validated Bedrock assets, but detection, launching, and add-on
  import are intentionally disabled until the supported runtime release.
  Bedrock ownership and online services will remain with the official
  Microsoft/Store app.
- Creator content remains subject to the creator's license and the applicable
  Modrinth or CurseForge terms. Review a project's information and version
  before installing it.

## Servers

Use **Servers** to add an existing server or create a local server. The create
wizard offers only tested server targets, asks for explicit Minecraft EULA
acceptance, and keeps server files separate from game profiles.

## AI Features

Amalgam includes built-in local AI that runs entirely on your PC.

### Creating an AI Profile
1. Open **Library** and create a new profile.
2. Enable **AI Profile** in the profile settings.
3. The AI brain (Qwen3-VL 8B) and vision model download automatically
   on first use (~5.5 GB total).

### Using AI Chat
1. Open the **AI** tab in your profile.
2. Type a message or select a mode:
   - **Ask**: Questions about Minecraft, mods, KubeJS, etc.
   - **Build**: Generate KubeJS scripts, datapacks, resource packs.
   - **Agent**: AI modifies project files with your approval.
   - **Auto**: AI chooses the best approach automatically.
3. Press Enter to send. The AI responds with code and explanations.

### Live Vision
1. Open Minecraft in a window.
2. Click **Start Live Vision** in the AI tab.
3. The AI describes what it sees on screen in real-time.
4. Ask questions about your world, builds, or gameplay.

### AI Art
1. Switch to **Art** mode in the AI tab.
2. Describe the texture or artwork you want.
3. The AI generates a Minecraft-compatible texture.
4. Save it to your profile's assets folder.

### Checkpoints and Undo
- AI file changes create automatic checkpoints.
- Click **Undo** to revert the last AI modification.
- View all checkpoints in the **History** panel.

## Troubleshooting

- Open **Settings** and run the readiness or prerequisite check when a launch
  option is unavailable.
- Use the profile's **Logs** tab for launch and content-install details.
- If an update or install is interrupted, reopen the profile or Downloads page
  and let Amalgam reconcile the operation before retrying.
- Do not share `launcher.json`, account tokens, or provider API keys with
  anyone. The release package includes a safe configuration template instead.
- If AI is not installed, go to **Settings > AI** and click **Install Amalgam AI**.

## Privacy

Profile files, logs, and optional diagnostics are stored locally. Optional
diagnostic sharing is off by default. Local AI processing stays on your PC.
See the installation information and project privacy materials for the
release-specific policy and support route.
