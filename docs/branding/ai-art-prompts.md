# Amalgam AI Art Pack

The launcher already supports `--ai-image <prompt> [out.png]`. Use the prompts
below with a configured image provider. Generate raster art without text; keep
the Amalgam wordmark and logo as the existing vector/approved assets.

## Shared art direction

**Style:** original voxel-fantasy game launcher art, cinematic lighting, deep
navy night palette, electric violet and magenta rim light, subtle teal accents,
clean focal subject, premium game-store composition, no UI, no watermark, no
letters, no logos, no copyrighted characters, no copied Minecraft screenshots.

**Negative prompt:** text, words, letters, logos, watermark, UI panels,
watermarks, photorealistic people, blurry subject, oversaturated rainbow,
existing game franchise characters, copied game screenshot, cropped focal point.

## Required surfaces

| Asset | Size | Prompt suffix |
|---|---:|---|
| `amalgam-discover-hero-ai.png` | 1600x620 | Wide fantasy voxel valley, floating violet portal, distant block-built towers, open negative space on the left for UI copy. |
| `profile-cover-vanilla-ai.png` | 1200x500 | Peaceful block-built meadow at dusk, river, mountains, warm windows, focal horizon centered. |
| `profile-cover-fabric-ai.png` | 1200x500 | Fast-moving violet energy ribbons through a futuristic voxel workshop, technical but friendly. |
| `profile-cover-forge-ai.png` | 1200x500 | Forge-lit underground voxel citadel, glowing crystal machinery, dramatic orange-violet contrast. |
| `profile-cover-neoforge-ai.png` | 1200x500 | Modern neon voxel city gate, violet power core, clean architectural geometry. |
| `profile-cover-quilt-ai.png` | 1200x500 | Quilted geometric portal garden, repeating violet patterns, calm exploration mood. |
| `profile-cover-bedrock-ai.png` | 1200x500 | Bright voxel ocean island with add-on inspired creatures represented as original silhouettes, violet sunrise. |
| `modpack-banner-ai.png` | 1600x600 | Epic original voxel modpack landscape with a strong central adventure path and clear edges for card cropping. |
| `server-card-ai.png` | 900x520 | Welcoming multiplayer voxel settlement at night, lit spawn plaza, violet beacon, no characters in close-up. |
| `onboarding-portal-ai.png` | 1200x800 | One luminous violet portal opening into a safe voxel landscape, hopeful first-run feeling, subject centered. |

## Generation and review

1. Run the launcher image command once per asset with the shared direction and
   the asset-specific prompt suffix.
2. Review at the launcher’s minimum window size and 125%/150% DPI.
3. Reject any image containing text, watermarks, recognizable copyrighted
   characters, or a focal point hidden by card overlays.
4. Keep the existing SVG/PNG fallback when provider generation is unavailable.
5. Record provider/model/date in the art manifest; never commit API keys.
