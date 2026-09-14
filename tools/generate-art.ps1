[CmdletBinding()]
param(
    [string]$Launcher = "$PSScriptRoot\..\cpp\build\amalgam_launcher.exe",
    [string]$OutputDir = "$PSScriptRoot\..\cpp\build\branding\ai",
    [switch]$Force
)

$ErrorActionPreference = "Stop"
if (-not (Test-Path -LiteralPath $Launcher)) {
    throw "Launcher not found: $Launcher. Build it first or pass -Launcher."
}
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

$shared = "original voxel-fantasy game launcher art, cinematic lighting, deep navy night palette, electric violet and magenta rim light, subtle teal accents, premium game-store composition, no UI, no watermark, no letters, no logos, no copyrighted characters, no copied Minecraft screenshots, text, words, letters, logos, watermark, UI panels, photorealistic people"
$assets = @(
    @{ Name = "amalgam-discover-hero-ai.png"; Size = "1600x620"; Prompt = "wide fantasy voxel valley, floating violet portal, distant block-built towers, open negative space on the left for UI copy" },
    @{ Name = "profile-cover-vanilla-ai.png"; Size = "1200x500"; Prompt = "peaceful block-built meadow at dusk, river, mountains, warm windows, centered horizon" },
    @{ Name = "profile-cover-fabric-ai.png"; Size = "1200x500"; Prompt = "violet energy ribbons through a futuristic voxel workshop, technical but friendly" },
    @{ Name = "profile-cover-forge-ai.png"; Size = "1200x500"; Prompt = "forge-lit underground voxel citadel, glowing crystal machinery, dramatic orange-violet contrast" },
    @{ Name = "profile-cover-neoforge-ai.png"; Size = "1200x500"; Prompt = "modern neon voxel city gate, violet power core, clean architectural geometry" },
    @{ Name = "profile-cover-quilt-ai.png"; Size = "1200x500"; Prompt = "geometric violet portal garden, repeating patterns, calm exploration mood" },
    @{ Name = "profile-cover-bedrock-ai.png"; Size = "1200x500"; Prompt = "bright voxel ocean island with original creature silhouettes, violet sunrise" },
    @{ Name = "modpack-banner-ai.png"; Size = "1600x600"; Prompt = "epic original voxel modpack landscape with a strong central adventure path and card-safe edges" },
    @{ Name = "server-card-ai.png"; Size = "900x520"; Prompt = "welcoming multiplayer voxel settlement at night, lit spawn plaza, violet beacon" },
    @{ Name = "onboarding-portal-ai.png"; Size = "1200x800"; Prompt = "one luminous violet portal opening into a safe voxel landscape, hopeful first-run feeling, centered subject" }
)

foreach ($asset in $assets) {
    $out = Join-Path $OutputDir $asset.Name
    if ((Test-Path -LiteralPath $out) -and -not $Force) {
        Write-Host "skip $($asset.Name) (already exists; use -Force to regenerate)"
        continue
    }
    $prompt = "$shared, $($asset.Prompt)"
    Write-Host "generate $($asset.Name)"
    & $Launcher --ai-image $prompt $out
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $out)) {
        throw "AI image generation failed for $($asset.Name). Check launcher.json AI provider configuration."
    }
}

Write-Host "Generated art is in $OutputDir"
Write-Host "Review all images for text, watermarks, copyrighted characters, cropping, and provider terms before packaging."
