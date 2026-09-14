param(
    [Parameter(Mandatory = $true)]
    [string]$Package,
    [string]$WorkRoot = (Join-Path $env:TEMP "amalgam-runtime-validation")
)

$ErrorActionPreference = "Stop"
$Package = (Resolve-Path $Package).Path
$stamp = [guid]::NewGuid().ToString("N")
$stage = Join-Path $WorkRoot $stamp
if (-not (Test-Path -LiteralPath $WorkRoot)) {
    New-Item -ItemType Directory -Path $WorkRoot -Force | Out-Null
}
New-Item -ItemType Directory -Path $stage -Force | Out-Null

try {
    & "C:\Windows\System32\tar.exe" -xf $Package -C $stage
    if ($LASTEXITCODE -ne 0) { throw "Archive extraction failed" }

    $launcher = Join-Path $stage "amalgam_launcher.exe"
    $dll = Join-Path $stage "amalgam.dll"
    if (-not (Test-Path -LiteralPath $launcher) -or -not (Test-Path -LiteralPath $dll)) {
        throw "Extracted package is missing native runtime artifacts"
    }
    $bedrockPackage = Join-Path $stage "bedrock\AmalgamBedrockClient.mcaddon"
    if (-not (Test-Path -LiteralPath $bedrockPackage)) {
        throw "Extracted package is missing the bundled Bedrock client"
    }
    $bedrockListing = (& "C:\Windows\System32\tar.exe" -tf $bedrockPackage 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0 -or
        $bedrockListing -notmatch [regex]::Escape("Amalgam Bedrock Behavior Pack.mcpack") -or
        $bedrockListing -notmatch [regex]::Escape("Amalgam Bedrock Resource Pack.mcpack")) {
        throw "Extracted Bedrock client package is incomplete"
    }
    foreach ($asset in @(
        "branding\ai\client-loading-ai.png",
        "branding\ai\client-menu-header-ai.png",
        "branding\ai\client-hud-icon-atlas-ai.png",
        "branding\ai\client-animation-fx-atlas-ai.png",
        "branding\ai\launcher-servers-ai.png",
        "branding\ai\launcher-essentials-ai.png",
        "branding\ai\launcher-performance-ai.png")) {
        if (-not (Test-Path -LiteralPath (Join-Path $stage $asset))) {
            throw "Extracted package is missing runtime art: $asset"
        }
    }

    Push-Location -LiteralPath $stage
    try {
        $javaOutput = (& $launcher --check-java 2>&1 | Out-String)
        if ($LASTEXITCODE -ne 0 -or $javaOutput -notmatch "Java") {
            throw "Clean package Java check failed: $javaOutput"
        }

        $prereqOutput = (& $launcher --check-prereqs 2>&1 | Out-String)
        if ($LASTEXITCODE -ne 0 -or $prereqOutput -notmatch "tar=OK") {
            throw "Clean package prerequisite check failed: $prereqOutput"
        }
    } finally {
        Pop-Location
    }

    $configFiles = @(Get-ChildItem -LiteralPath $stage -Recurse -Filter "launcher.json" -File -ErrorAction SilentlyContinue)
    foreach ($config in $configFiles) {
        $text = [IO.File]::ReadAllText($config.FullName, [Text.Encoding]::UTF8)
        if ($text -match '"(api_key|access_token|refresh_token|curseforge_key|modrinth_token)"\s*:') {
            throw "Runtime smoke created a secret-bearing configuration: $($config.FullName)"
        }
    }

    Write-Output "Clean extracted-package runtime checks passed: $Package"
} finally {
    if (Test-Path -LiteralPath $stage) {
        Remove-Item -LiteralPath $stage -Recurse -Force
    }
}
