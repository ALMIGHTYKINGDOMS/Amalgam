param(
    [string]$BuildDir = (Join-Path $PSScriptRoot "..\cpp\build"),
    [string]$OutputDir = (Join-Path $PSScriptRoot "..\dist"),
    [string]$Version = "local",
    [string]$MicrosoftClientId = $env:AMALGAM_MICROSOFT_CLIENT_ID,
    [string]$SupabaseUrl = $env:AMALGAM_SUPABASE_URL,
    [string]$SupabasePublishableKey = $env:AMALGAM_SUPABASE_PUBLISHABLE_KEY,
    [string]$WebsiteUrl = $env:AMALGAM_WEBSITE_URL,
    [string]$ApiUrl = $env:AMALGAM_API_URL,
    [switch]$RequireOnlineConfig,
    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"
$BuildDir = (Resolve-Path $BuildDir).Path
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
$stage = Join-Path $OutputDir "amalgam-$Version"
$zip = Join-Path $OutputDir "amalgam-$Version.zip"

if ([string]::IsNullOrWhiteSpace($WebsiteUrl)) { $WebsiteUrl = "https://amalgam-mc.com/" }
if (-not [string]::IsNullOrWhiteSpace($MicrosoftClientId) -and
    $MicrosoftClientId -notmatch '^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$') {
    throw "Microsoft client ID must be an application UUID"
}
if (-not [string]::IsNullOrWhiteSpace($SupabaseUrl) -and
    $SupabaseUrl -notmatch '^https://[a-z0-9-]+\.supabase\.co/?$') {
    throw "Supabase URL must be an https://<project>.supabase.co address"
}
if ([string]::IsNullOrWhiteSpace($SupabaseUrl) -xor [string]::IsNullOrWhiteSpace($SupabasePublishableKey)) {
    throw "Supabase URL and publishable key must be supplied together"
}
if ($RequireOnlineConfig) {
    $missingPublicConfig = @()
    if ([string]::IsNullOrWhiteSpace($MicrosoftClientId)) { $missingPublicConfig += "Microsoft client ID" }
    if ([string]::IsNullOrWhiteSpace($SupabaseUrl)) { $missingPublicConfig += "Supabase URL" }
    if ([string]::IsNullOrWhiteSpace($SupabasePublishableKey)) { $missingPublicConfig += "Supabase publishable key" }
    if ($missingPublicConfig.Count -gt 0) {
        throw "Official package is missing public configuration: $($missingPublicConfig -join ', ')"
    }
}

$required = @("amalgam_launcher.exe", "amalgam.dll")
foreach ($name in $required) {
    $path = Join-Path $BuildDir $name
    if (-not (Test-Path -LiteralPath $path)) { throw "Missing build artifact: $path" }
}

$bridges = @(Get-ChildItem -LiteralPath (Join-Path $BuildDir "bridges") -Filter "*.jar" -File |
    Where-Object { $_.Name -notlike "*-sources.jar" })
if ($bridges.Count -eq 0) { throw "No runtime bridge jars found" }
$expected_bridges = @(
    "amalgam-fabric-1.18.2.jar", "amalgam-fabric-1.19.2.jar", "amalgam-fabric-1.20.1.jar",
    "amalgam-fabric-1.21.1.jar", "amalgam-fabric-1.21.4.jar", "amalgam-fabric-1.21.5.jar",
    "amalgam-fabric-1.21.6.jar", "amalgam-fabric-1.21.8.jar", "amalgam-fabric-1.21.11.jar",
    "amalgam-neoforge-1.21.1.jar", "amalgam-neoforge-1.21.4.jar", "amalgam-neoforge-1.21.5.jar",
    "amalgam-neoforge-1.21.6.jar", "amalgam-neoforge-1.21.8.jar", "amalgam-neoforge-1.21.11.jar",
    "amalgam-forge-1.12.2.jar", "amalgam-forge-1.18.2.jar", "amalgam-forge-1.19.2.jar",
    "amalgam-forge-1.20.1.jar"
)
$actual_bridge_names = @($bridges | ForEach-Object { $_.Name })
$missing_bridges = @($expected_bridges | Where-Object { $_ -notin $actual_bridge_names })
$unexpected_bridges = @($actual_bridge_names | Where-Object { $_ -notin $expected_bridges })
if ($missing_bridges.Count -gt 0 -or $unexpected_bridges.Count -gt 0) {
    throw "Bridge set mismatch. Missing: $($missing_bridges -join ', '); unexpected: $($unexpected_bridges -join ', ')"
}

# Fail closed when a bridge jar in the build tree is older than its gradle
# build output, so a package can never ship stale bridges again.
$bridge_gradle_outputs = @{
    "amalgam-fabric-1.18.2.jar"    = "java/fabric-1.18.2/build/libs"
    "amalgam-fabric-1.19.2.jar"    = "java/fabric-1.19.2/build/libs"
    "amalgam-fabric-1.20.1.jar"    = "java/fabric-1.20.1/build/libs"
    "amalgam-fabric-1.21.1.jar"    = "java/fabric-1.21.1/build/libs"
    "amalgam-fabric-1.21.4.jar"    = "java/fabric-1.21.4/build/libs"
    "amalgam-fabric-1.21.5.jar"    = "java/fabric-1.21.5/build/libs"
    "amalgam-fabric-1.21.6.jar"    = "java/fabric-1.21.6/build/libs"
    "amalgam-fabric-1.21.8.jar"    = "java/fabric-1.21.8/build/libs"
    "amalgam-fabric-1.21.11.jar"   = "java/fabric-1.21.11/build/libs"
    "amalgam-forge-1.12.2.jar"     = "java-forge-1.12.2/build/libs"
    "amalgam-forge-1.18.2.jar"     = "java-forge-legacy/forge-1.18.2/build/libs"
    "amalgam-forge-1.19.2.jar"     = "java-forge-legacy/forge-1.19.2/build/libs"
    "amalgam-forge-1.20.1.jar"     = "java-forge/build/libs"
    "amalgam-neoforge-1.21.1.jar"  = "java-neoforge/neoforge-1.21.1/build/libs"
    "amalgam-neoforge-1.21.4.jar"  = "java-neoforge/neoforge-1.21.4/build/libs"
    "amalgam-neoforge-1.21.5.jar"  = "java-neoforge/neoforge-1.21.5/build/libs"
    "amalgam-neoforge-1.21.6.jar"  = "java-neoforge/neoforge-1.21.6/build/libs"
    "amalgam-neoforge-1.21.8.jar"  = "java-neoforge/neoforge-1.21.8/build/libs"
    "amalgam-neoforge-1.21.11.jar" = "java-neoforge/neoforge-1.21.11/build/libs"
}
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$stale_bridges = @()
foreach ($b in $bridges) {
    $gradleDir = $bridge_gradle_outputs[$b.Name]
    if (-not $gradleDir) { continue }
    $built = Join-Path $repoRoot (Join-Path $gradleDir $b.Name)
    if ((Test-Path -LiteralPath $built) -and
        ((Get-Item -LiteralPath $built).LastWriteTimeUtc -gt $b.LastWriteTimeUtc)) {
        $stale_bridges += "$( $b.Name ) -> $built"
    }
}
if ($stale_bridges.Count -gt 0) {
    throw ("Stale bridge jars in build directory. Copy the newer gradle outputs into " +
           (Join-Path $BuildDir "bridges") + " and repackage: " + ($stale_bridges -join '; '))
}

$bedrockDir = Join-Path $BuildDir "bedrock"
$bedrockPackage = Join-Path $bedrockDir "AmalgamBedrockClient.mcaddon"
if (-not (Test-Path -LiteralPath $bedrockPackage)) {
    throw "Validated Bedrock client package is missing: $bedrockPackage"
}
$bedrockInventoryPath = Join-Path $bedrockDir "package-inventory.json"
$bedrockHashPath = Join-Path $bedrockDir "package-sha256.txt"
if (-not (Test-Path -LiteralPath $bedrockInventoryPath) -or
    -not (Test-Path -LiteralPath $bedrockHashPath)) {
    throw "Bedrock package metadata is missing: expected package-inventory.json and package-sha256.txt"
}
$bedrockInventory = Get-Content -LiteralPath $bedrockInventoryPath -Raw | ConvertFrom-Json
if ($bedrockInventory.version -ne $Version -or
    $bedrockInventory.package -ne "AmalgamBedrockClient-$Version.mcaddon") {
    throw "Bedrock package metadata version mismatch. Expected $Version, found $($bedrockInventory.version)"
}
$bedrockHashLine = (Get-Content -LiteralPath $bedrockHashPath | Select-Object -First 1)
if ($bedrockHashLine -notmatch [regex]::Escape("AmalgamBedrockClient-$Version.mcaddon") + '$') {
    throw "Bedrock package hash metadata version mismatch for $Version"
}
$bedrockListing = (& "C:\Windows\System32\tar.exe" -tf $bedrockPackage 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0 -or
    $bedrockListing -notmatch [regex]::Escape("Amalgam Bedrock Behavior Pack.mcpack") -or
    $bedrockListing -notmatch [regex]::Escape("Amalgam Bedrock Resource Pack.mcpack")) {
    throw "Bedrock client package is missing one or more pack archives"
}

if (-not $SkipTests) {
    $ctest = Get-Command ctest -ErrorAction SilentlyContinue
    if (-not $ctest) {
        $known = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
        if (Test-Path -LiteralPath $known) { $ctest = Get-Item -LiteralPath $known }
    }
    if (-not $ctest) { throw "ctest.exe was not found" }
    $ctestPath = if ($ctest.Source) { $ctest.Source } else { $ctest.FullName }
    & $ctestPath --test-dir $BuildDir --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "CTest failed" }
}

if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
New-Item -ItemType Directory -Path (Join-Path $stage "bridges") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $stage "branding") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $stage "bedrock") -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $BuildDir "amalgam_launcher.exe") -Destination $stage
Copy-Item -LiteralPath (Join-Path $BuildDir "amalgam.dll") -Destination $stage
Copy-Item -LiteralPath $bedrockPackage -Destination (Join-Path $stage "bedrock")
foreach ($metadata in @("package-inventory.json", "package-sha256.txt")) {
    $metadataPath = Join-Path (Split-Path -Parent $bedrockPackage) $metadata
    if (Test-Path -LiteralPath $metadataPath) {
        Copy-Item -LiteralPath $metadataPath -Destination (Join-Path $stage "bedrock")
    }
}
foreach ($bridge in $bridges) {
    Copy-Item -LiteralPath $bridge.FullName -Destination (Join-Path $stage "bridges")
}
$branding_dir = Join-Path $PSScriptRoot "..\docs\branding"
if (Test-Path -LiteralPath $branding_dir) {
    $branding_dir = (Resolve-Path -LiteralPath $branding_dir).Path
    # Branding is recursive: the launcher and in-game client both consume the
    # generated art under branding/ai. Preserve the relative directory shape
    # so packaged builds resolve the same paths as the development build.
    Get-ChildItem -LiteralPath $branding_dir -Recurse -File |
        Where-Object { $_.Extension -in @(".svg", ".png", ".json", ".md") } |
        ForEach-Object {
            $relative = $_.FullName.Substring($branding_dir.Length + 1)
            $destination = Join-Path $stage (Join-Path "branding" $relative)
            $destination_dir = Split-Path -Parent $destination
            New-Item -ItemType Directory -Path $destination_dir -Force | Out-Null
            Copy-Item -LiteralPath $_.FullName -Destination $destination -Force
        }
}
$user_guide = Join-Path $PSScriptRoot "..\docs\USER-GUIDE.md"
if (-not (Test-Path -LiteralPath $user_guide)) {
    throw "User guide is missing: $user_guide"
}
Copy-Item -LiteralPath $user_guide -Destination $stage
$release_notes = Join-Path $PSScriptRoot "..\docs\RELEASE-NOTES.md"
if (-not (Test-Path -LiteralPath $release_notes)) {
    throw "Release notes are missing: $release_notes"
}
Copy-Item -LiteralPath $release_notes -Destination $stage
$beta_tester_guide = Join-Path $PSScriptRoot "..\docs\beta-tester-guide.md"
if (-not (Test-Path -LiteralPath $beta_tester_guide)) {
    throw "Beta tester guide is missing: $beta_tester_guide"
}
Copy-Item -LiteralPath $beta_tester_guide -Destination (Join-Path $stage "BETA-TESTER-GUIDE.md")
$installer_docs = @(
    @{ source = (Join-Path $PSScriptRoot "..\installer\LICENSE.txt"); name = "LICENSE.txt" },
    @{ source = (Join-Path $PSScriptRoot "..\installer\INSTALL-INFO.txt"); name = "INSTALL-INFO.txt" }
)
foreach ($doc in $installer_docs) {
    if (-not (Test-Path -LiteralPath $doc.source)) {
        throw "Release documentation is missing: $($doc.source)"
    }
    Copy-Item -LiteralPath $doc.source -Destination (Join-Path $stage $doc.name)
}

# The launcher is intentionally small, but it must still be self-contained:
# package the verifier/bootstrapper, pinned public manifests, and the native
# runtimes. Large model weights are never copied here; the bootstrapper fetches
# them into the per-user AI directory and verifies them before activation.
$repo_root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$ai_stage = Join-Path $stage "ai"
$tools_stage = Join-Path $stage "tools"
$legal_stage = Join-Path $stage "legal"
New-Item -ItemType Directory -Path $ai_stage, $tools_stage, $legal_stage -Force | Out-Null
foreach ($file in @("ai-manifest.json", "ai-package-manifest.json")) {
    $source = Join-Path $repo_root (Join-Path "ai" $file)
    if (-not (Test-Path -LiteralPath $source)) { throw "AI manifest is missing: $source" }
    Copy-Item -LiteralPath $source -Destination (Join-Path $ai_stage $file)
}
$knowledge_source = Join-Path $repo_root "ai\knowledge"
if (-not (Test-Path -LiteralPath $knowledge_source)) { throw "AI knowledge directory is missing: $knowledge_source" }
Get-ChildItem -LiteralPath $knowledge_source -Recurse -File | ForEach-Object {
    $relative = $_.FullName.Substring($knowledge_source.Length + 1)
    $destination = Join-Path $ai_stage (Join-Path "knowledge" $relative)
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $_.FullName -Destination $destination -Force
}
foreach ($file in @("ai-bootstrap.ps1", "ai-bootstrap.cmd")) {
    $source = Join-Path $repo_root (Join-Path "tools" $file)
    if (-not (Test-Path -LiteralPath $source)) { throw "AI bootstrapper is missing: $source" }
    Copy-Item -LiteralPath $source -Destination (Join-Path $tools_stage $file)
}
foreach ($file in @("TERMS.txt", "PRIVACY.txt", "AI_TERMS.txt", "THIRD_PARTY_NOTICES.txt")) {
    $source = Join-Path $repo_root (Join-Path "legal" $file)
    if (-not (Test-Path -LiteralPath $source)) { throw "Legal document is missing: $source" }
    Copy-Item -LiteralPath $source -Destination (Join-Path $legal_stage $file)
}

$llama_source = Join-Path $repo_root "third_party\ai\llama.cpp\build-cpu\bin"
$llama_stage = Join-Path $stage "runtimes\ai\llama"
$llama_executables = @("llama-completion.exe", "llama-mtmd-cli.exe")
if (-not (Test-Path -LiteralPath $llama_source)) { throw "Built llama.cpp runtime is missing: $llama_source" }
New-Item -ItemType Directory -Path $llama_stage -Force | Out-Null
foreach ($file in $llama_executables) {
    $source = Join-Path $llama_source $file
    if (-not (Test-Path -LiteralPath $source)) { throw "Required llama.cpp executable is missing: $source" }
    Copy-Item -LiteralPath $source -Destination (Join-Path $llama_stage $file)
}
Get-ChildItem -LiteralPath $llama_source -Filter "*.dll" -File | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $llama_stage $_.Name) -Force
}

$sd_source = Join-Path $repo_root "third_party\ai\stable-diffusion.cpp\build-cli\bin"
$sd_stage = Join-Path $stage "runtimes\ai\sd"
if (-not (Test-Path -LiteralPath $sd_source)) { throw "Built stable-diffusion.cpp CLI runtime is missing: $sd_source" }
$sd_cli = Join-Path $sd_source "sd-cli.exe"
if (-not (Test-Path -LiteralPath $sd_cli)) {
    # The source build currently produces the CLI as sd-cli.exe only in some
    # configurations. Never silently ship an incomplete AI package: accept the
    # already-built stable-diffusion.exe alias when present, otherwise fail.
    $sd_cli = Join-Path $sd_source "stable-diffusion.exe"
}
if (-not (Test-Path -LiteralPath $sd_cli)) { throw "Required stable-diffusion CLI is missing: $sd_source (expected sd-cli.exe or stable-diffusion.exe)" }
New-Item -ItemType Directory -Path $sd_stage -Force | Out-Null
Copy-Item -LiteralPath $sd_cli -Destination (Join-Path $sd_stage "stable-diffusion.exe")
Get-ChildItem -LiteralPath $sd_source -Filter "*.dll" -File | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $sd_stage $_.Name) -Force
}

$templateData = [ordered]@{
    base_dir = "instances"
    assets_dir = "assets"
    java_cache_dir = "runtimes/java"
    bridges_dir = "bridges"
    loader = "auto"
    performance_profile = "auto"
    microsoft_client_id = $MicrosoftClientId
    supabase_url = $SupabaseUrl
    supabase_anon_key = $SupabasePublishableKey
    supabase_service_key = ""
    website_url = $WebsiteUrl
    api_url = $ApiUrl
    username = ""
    ai_providers = @()
}
$template = $templateData | ConvertTo-Json -Depth 4
$utf8 = New-Object System.Text.UTF8Encoding($false)
[IO.File]::WriteAllText((Join-Path $stage "launcher.json.template"), $template, $utf8)
$prerequisites = @'
{
  "platform": "Windows 10+ x64",
  "launcher_checks": ["tar.exe", "winsqlite3.dll", "VCRUNTIME140.dll", "VCRUNTIME140_1.dll", "MSVCP140.dll"],
  "diagnostics": "SQLite persistence is opt-in, but the Windows 10+ winsqlite3 system component is required by the native DLL"
} 
'@
[IO.File]::WriteAllText((Join-Path $stage "prerequisites.json"), $prerequisites, $utf8)

$components = @(
    Get-ChildItem -LiteralPath $stage -Recurse -File |
        Where-Object { $_.Name -notin @("component-manifest.json", "release.sha256") } |
        Sort-Object FullName |
        ForEach-Object {
            $hash = Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256
            [PSCustomObject]@{
                name = $_.FullName.Substring($stage.Length + 1).Replace("\", "/")
                sha256 = $hash.Hash.ToLowerInvariant()
                size = $_.Length
            }
        }
)
[IO.File]::WriteAllText((Join-Path $stage "component-manifest.json"),
    ($components | ConvertTo-Json -Depth 3), $utf8)

$sbomComponents = @(
    $components | ForEach-Object {
        [PSCustomObject]@{
            type = if ($_.name -like "bridges/*") { "library" } else { "application" }
            name = $_.name
            version = $Version
            hashes = @([PSCustomObject]@{ alg = "SHA-256"; content = $_.sha256 })
        }
    }
)
$sbom = [ordered]@{
    bomFormat = "CycloneDX"
    specVersion = "1.5"
    serialNumber = "urn:uuid:$([guid]::NewGuid())"
    version = 1
    metadata = [ordered]@{
        timestamp = [DateTime]::UtcNow.ToString("o")
        tools = @([ordered]@{ vendor = "Amalgam"; name = "package-release.ps1"; version = "1" })
    }
    components = $sbomComponents
}
[IO.File]::WriteAllText((Join-Path $stage "sbom.cdx.json"),
    ($sbom | ConvertTo-Json -Depth 8), $utf8)

$hashLines = @()
Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName | ForEach-Object {
    $hash = Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256
    $relative = $_.FullName.Substring($stage.Length + 1).Replace("\", "/")
    $hashLines += "$($hash.Hash.ToLowerInvariant())  $relative"
}
[IO.File]::WriteAllLines((Join-Path $stage "release.sha256"), $hashLines, $utf8)
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip -CompressionLevel Optimal
Write-Output "Packaged $zip"
