param(
    [Parameter(Mandatory = $true)]
    [string]$Package,
    [string]$WorkRoot = (Join-Path $env:TEMP "amalgam-package-validation"),
    [switch]$RequireOnlineConfig
)

$ErrorActionPreference = "Stop"
$Package = (Resolve-Path $Package).Path
$stamp = [guid]::NewGuid().ToString("N")
$stage = Join-Path $WorkRoot $stamp
New-Item -ItemType Directory -Path $stage -Force | Out-Null
try {
    & "C:\Windows\System32\tar.exe" -xf $Package -C $stage
    if ($LASTEXITCODE -ne 0) { throw "Archive extraction failed" }

    $launcher = Join-Path $stage "amalgam_launcher.exe"
    $dll = Join-Path $stage "amalgam.dll"
    if (-not (Test-Path -LiteralPath $launcher) -or -not (Test-Path -LiteralPath $dll)) {
        throw "Native launcher artifacts are missing"
    }
    $bedrockPackage = Join-Path $stage "bedrock\AmalgamBedrockClient.mcaddon"
    if (-not (Test-Path -LiteralPath $bedrockPackage)) {
        throw "Bedrock client package is missing from the release"
    }
    $bedrockListing = (& "C:\Windows\System32\tar.exe" -tf $bedrockPackage 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0 -or
        $bedrockListing -notmatch [regex]::Escape("Amalgam Bedrock Behavior Pack.mcpack") -or
        $bedrockListing -notmatch [regex]::Escape("Amalgam Bedrock Resource Pack.mcpack")) {
        throw "Bedrock client package is malformed or incomplete"
    }
    $jars = @(Get-ChildItem -LiteralPath (Join-Path $stage "bridges") -Filter "*.jar" -File)
    if ($jars.Count -ne 19) { throw "Expected 19 runtime bridge jars, found $($jars.Count)" }
    foreach ($branding in @("amalgam-mark.svg", "amalgam-banner.svg", "amalgam-logo.png", "amalgam-banner.png",
                            "ai-art-manifest.json", "ai\client-loading-ai.png",
                            "ai\client-menu-header-ai.png", "ai\client-hud-icon-atlas-ai.png",
                            "ai\client-animation-fx-atlas-ai.png", "ai\launcher-servers-ai.png",
                            "ai\launcher-essentials-ai.png", "ai\launcher-performance-ai.png",
                            "ai\amalgam-discover-hero-ai.png", "ai\modpack-banner-ai.png")) {
        if (-not (Test-Path -LiteralPath (Join-Path $stage "branding\$branding"))) {
            throw "Missing branding asset: $branding"
        }
    }
    $artManifest = Join-Path $stage "branding\ai-art-manifest.json"
    $artEntries = Get-Content -LiteralPath $artManifest -Raw | ConvertFrom-Json
    foreach ($entry in @($artEntries.assets)) {
        $relative = [string]$entry.file
        if ([string]::IsNullOrWhiteSpace($relative)) { throw "Malformed AI art manifest entry" }
        $artPath = Join-Path $stage (Join-Path "branding" ($relative.Replace("/", "\")))
        if (-not (Test-Path -LiteralPath $artPath)) { throw "AI art manifest asset is missing: $relative" }
    }
    if (@($jars | Where-Object { $_.Name -like "*-sources.jar" }).Count -ne 0) {
        throw "Source jars must not be packaged"
    }
    $required = @(
        "amalgam-fabric-1.18.2.jar", "amalgam-fabric-1.19.2.jar", "amalgam-fabric-1.20.1.jar",
        "amalgam-fabric-1.21.1.jar", "amalgam-fabric-1.21.4.jar", "amalgam-fabric-1.21.5.jar",
        "amalgam-fabric-1.21.6.jar", "amalgam-fabric-1.21.8.jar", "amalgam-fabric-1.21.11.jar",
        "amalgam-neoforge-1.21.1.jar", "amalgam-neoforge-1.21.4.jar", "amalgam-neoforge-1.21.5.jar",
        "amalgam-neoforge-1.21.6.jar", "amalgam-neoforge-1.21.8.jar", "amalgam-neoforge-1.21.11.jar",
        "amalgam-forge-1.12.2.jar", "amalgam-forge-1.18.2.jar", "amalgam-forge-1.19.2.jar",
        "amalgam-forge-1.20.1.jar"
    )
    foreach ($name in $required) {
        if (-not (Test-Path -LiteralPath (Join-Path $stage "bridges\$name"))) { throw "Missing bridge: $name" }
    }
    $templatePath = Join-Path $stage "launcher.json.template"
    if (-not (Test-Path -LiteralPath $templatePath)) { throw "Public launcher configuration template is missing" }
    foreach ($releaseDoc in @("LICENSE.txt", "INSTALL-INFO.txt", "USER-GUIDE.md", "RELEASE-NOTES.md", "BETA-TESTER-GUIDE.md")) {
        if (-not (Test-Path -LiteralPath (Join-Path $stage $releaseDoc))) {
            throw "Release documentation is missing: $releaseDoc"
        }
    }
    $templateConfig = Get-Content -LiteralPath $templatePath -Raw | ConvertFrom-Json
    if (-not [string]::IsNullOrWhiteSpace([string]$templateConfig.supabase_service_key)) {
        throw "Supabase service-role credentials must never ship in the launcher"
    }
    $publicOnline = @(
        [string]$templateConfig.microsoft_client_id,
        [string]$templateConfig.supabase_url,
        [string]$templateConfig.supabase_anon_key
    )
    $configuredOnline = @($publicOnline | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }).Count
    if (($configuredOnline -gt 0 -and $configuredOnline -ne $publicOnline.Count) -or
        ($RequireOnlineConfig -and $configuredOnline -ne $publicOnline.Count)) {
        throw "Public online configuration is incomplete"
    }
    $allFiles = @(Get-ChildItem -LiteralPath $stage -Recurse -File)
    foreach ($file in $allFiles) {
        if ($file.Name -eq "launcher.json" -or
            $file.Extension -in @(".hprof", ".dmp", ".part", ".pdb", ".log")) {
            throw "Forbidden release artifact: $($file.FullName)"
        }
        if ($file.Extension -notin @(".json", ".txt", ".toml", ".md", ".ps1")) { continue }
        $text = [IO.File]::ReadAllText($file.FullName, [Text.Encoding]::UTF8)
        if ($text -match '"(api_key|access_token|refresh_token|curseforge_key|modrinth_token)"\s*:') {
            throw "Possible secret-bearing file: $($file.FullName)"
        }
    }
    $componentManifest = Join-Path $stage "component-manifest.json"
    if (-not (Test-Path -LiteralPath $componentManifest)) { throw "Component manifest is missing" }
    # ConvertFrom-Json returns a single Object[] for a JSON array in Windows
    # PowerShell.  Keep that array intact so foreach iterates its entries
    # rather than treating the whole manifest as one component.
    $components = Get-Content -LiteralPath $componentManifest -Raw | ConvertFrom-Json
    foreach ($component in $components) {
        $relative = [string]($component.name)
        $expected = [string]($component.sha256)
        if ([string]::IsNullOrWhiteSpace($relative) -or $expected -notmatch '^[0-9a-fA-F]{64}$') {
            throw "Malformed component manifest entry"
        }
        $relative = $relative.Replace("/", "\\")
        if ([IO.Path]::IsPathRooted($relative) -or $relative -match '(^|\\)\.\.(\\|$)') {
            throw "Unsafe component path: $relative"
        }
        $file = [IO.Path]::GetFullPath((Join-Path $stage $relative))
        $stageRoot = [IO.Path]::GetFullPath($stage)
        if (-not $stageRoot.EndsWith('\')) { $stageRoot += '\' }
        if (-not $file.StartsWith($stageRoot, [System.StringComparison]::OrdinalIgnoreCase) -or
            -not (Test-Path -LiteralPath $file)) {
            throw "Component path is missing or escapes package: $relative"
        }
        if ([int64]($component.size) -ne (Get-Item -LiteralPath $file).Length) {
            throw "Component size mismatch: $relative"
        }
        $actual = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -ne $expected.ToLowerInvariant()) { throw "Component hash mismatch: $relative" }
    }
    $hashFile = Join-Path $stage "release.sha256"
    if (-not (Test-Path -LiteralPath $hashFile)) { throw "release.sha256 is missing" }
    foreach ($line in Get-Content -LiteralPath $hashFile) {
        if ($line -notmatch '^([0-9a-fA-F]{64})\s+(.+)$') { throw "Malformed hash line: $line" }
        $expected = $Matches[1].ToLowerInvariant()
        $relative = $Matches[2].Replace("/", "\")
        if ([IO.Path]::IsPathRooted($relative) -or $relative -match '(^|\\)\.\.(\\|$)') {
            throw "Unsafe hashed path: $relative"
        }
        $file = [IO.Path]::GetFullPath((Join-Path $stage $relative))
        $stageRoot = [IO.Path]::GetFullPath($stage)
        if (-not $stageRoot.EndsWith('\')) { $stageRoot += '\' }
        if (-not $file.StartsWith($stageRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Hashed path escapes package: $relative"
        }
        if (-not (Test-Path -LiteralPath $file)) { throw "Hashed file is missing: $relative" }
        $actual = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -ne $expected) { throw "Hash mismatch: $relative" }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $stage "sbom.cdx.json"))) { throw "SBOM is missing" }
    & $launcher --check-prereqs | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Packaged prerequisite check failed" }
    Write-Output "Package validation passed: $Package"
}
finally {
    Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
}
