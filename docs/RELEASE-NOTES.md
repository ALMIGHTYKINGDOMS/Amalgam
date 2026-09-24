# Amalgam 1.0.0

## What's New

Amalgam is a premium Minecraft launcher that unifies Java Edition, profiles, content discovery, local servers, and Essentials multiplayer sessions in a single desktop application with built-in local AI. Bedrock packaging is prepared for a future release and is visibly marked **Coming Soon** in 1.0.0.

### AI Features (New in 1.0.0)

- **Local AI Brain**: Qwen3-VL 8B for chat, coding, and reasoning — runs entirely on your PC
- **Live Vision**: Capture your screen and ask the AI to describe what it sees
- **AI Art**: Generate Minecraft textures, logos, and artwork using FLUX 2 Klein 4B
- **AI Profiles**: Create dedicated AI-powered modpack profiles
- **Agent Mode**: AI can modify project files with checkpoint/undo safety
- **Build Mode**: Auto-save generated KubeJS scripts, datapacks, and resource packs
- **Streaming Responses**: AI replies appear word-by-word as they generate
- **Offline AI**: All AI features work without internet after installation
- **No External Tools**: No Ollama, LM Studio, ComfyUI, or cloud APIs required

### Core Features

- **Unified Launcher**: Java Edition, profiles, content, local servers, and Essentials sessions; Bedrock is a clearly gated Coming Soon surface
- **Profile Management**: Create, import, duplicate, rename, and manage Minecraft profiles with version and loader selection
- **Content Discovery**: Browse and install mods, modpacks, shaders, and resource packs from Modrinth and CurseForge
- **Download Manager**: Track download progress, speed, and status with pause/resume/cancel support
- **Local Servers**: Create and manage local Minecraft servers with console, player management, and file access
- **Bedrock Coming Soon**: static package and manifest validation are complete; Windows detection, launch, add-on import, and client integration are held for the supported runtime release
- **Java Runtime Management**: Managed Java detection, download, and selection per profile or server
- **Settings & Diagnostics**: Comprehensive settings with runtime health checks for all systems

### Performance Optimizations

- **Instant Startup**: CJK font merge deferred to post-first-frame rendering
- **Faster Fonts**: Reduced glyph oversampling (33% smaller font atlas)
- **LTO Build**: Whole-program optimization for 10-15% smaller binary
- **Deferred Network**: Startup HTTP requests fire after UI renders

### Supported Platforms

- **Java Edition**: Vanilla, Fabric, Forge, NeoForge (Quilt experimental)
- **Bedrock**: Coming Soon (static package validation only in 1.0.0)
- **Essentials**: Friends, invites, sessions, messages, parties, notifications
- **Local Servers**: Vanilla, Paper, Spigot, Purpur, Fabric, Forge, NeoForge

### System Requirements

- **OS**: Windows 10/11 (64-bit)
- **RAM**: 4 GB minimum, 8 GB recommended for AI
- **Storage**: 2 GB for launcher, 15 GB with AI models
- **GPU**: Any (CPU fallback available for AI)
- **Java**: Managed by Amalgam (no manual installation required)
- **Network**: Required for downloads, AI works offline after install

### Installer

- **Size**: 76 MB (small online installer)
- **AI Download**: ~11.6 GB automatic during setup
- **Components**: Brain (5.4 GB), Vision (0.8 GB), Art (2.6 GB), Encoder (2.7 GB), Decoder (0.3 GB)
- **Resume Support**: Interrupted downloads resume automatically
- **SHA-256 Verified**: Every component verified after download

### Important Notes

- Microsoft account authentication for Java Edition requires Minecraft ownership
- Bedrock runtime functionality is not enabled in 1.0.0; the supported Windows integration will require Minecraft for Windows when that release is published
- Essentials multiplayer requires an Amalgam account
- Cloud hosting remains gated and is not included in this release
- AI models download automatically during installation
- Review the supplied Terms, Privacy, AI Terms, and Third-Party Notices before distribution

### Known Issues

- Quilt loader support is experimental
- Bedrock runtime actions are intentionally disabled until the supported integration release
- TURN relay fallback may have latency on slow connections
- AI art generation requires first-time model download (~5 GB)

### Support

- **Website**: https://amalgam-mc.com
- **Discord**: https://discord.gg/amalgam
- **Bug Reports**: Use the in-app "Send feedback" button
- **Email**: support@amalgam-mc.com

### Legal

- **License**: See LICENSE.txt in the installation directory
- **Privacy**: https://amalgam-mc.com/privacy
- **Terms**: https://amalgam-mc.com/terms
- **AI Terms**: See legal/AI_TERMS.txt
- **Third-Party**: See legal/THIRD_PARTY_NOTICES.txt
