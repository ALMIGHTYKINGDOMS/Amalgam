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
    [switch]$SkipTests,
    # Regenerate only the derived package metadata and ZIP from an existing
    # staged payload. This is the only safe post-signing path: it never copies
    # fresh unsigned binaries over the signed stage.
    [switch]$FinalizeExistingStage,
    # Intended for the production signing sequence. It verifies that the two
    # staged native binaries are authenticode-valid before their new hashes and
    # the final ZIP are written.
    [switch]$RequireSignedStagedBinaries
)

$ErrorActionPreference = "Stop"
if ($Version -notmatch '^[0-9A-Za-z][0-9A-Za-z._-]*$') {
    throw "Version may contain only letters, digits, dots, underscores, and hyphens"
}
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
$stage = [IO.Path]::GetFullPath((Join-Path $OutputDir "amalgam-$Version"))
$zip = [IO.Path]::GetFullPath((Join-Path $OutputDir "amalgam-$Version.zip"))
$utf8 = New-Object System.Text.UTF8Encoding($false)

function Assert-ReleaseArtifactPath([string]$Path, [string]$Name) {
    $rootPath = [IO.Path]::GetFullPath($OutputDir)
    if (-not $rootPath.EndsWith([IO.Path]::DirectorySeparatorChar)) {
        $rootPath += [IO.Path]::DirectorySeparatorChar
    }
    if (-not $Path.StartsWith($rootPath, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "$Name escapes the requested output directory"
    }
}

function Remove-DerivedReleaseFile([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Expected a file at derived artifact path, found a directory: $Path"
    }
    Remove-Item -LiteralPath $Path -Force
}

function Assert-StagedNativeBinaries([string]$StageDir, [switch]$RequireSignatures) {
    foreach ($name in @("amalgam_launcher.exe", "amalgam.dll")) {
        $path = Join-Path $StageDir $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Staged native artifact is missing: $path"
        }
        if ((Get-Item -LiteralPath $path).Length -le 0) {
            throw "Staged native artifact is empty: $path"
        }
        if ($RequireSignatures) {
            $signature = Get-AuthenticodeSignature -LiteralPath $path
            if ($signature.Status -ne "Valid") {
                throw "Staged native artifact is not validly signed: $name ($($signature.Status))"
            }
        }
    }
}

function Assert-BedrockPackageIntegrity(
    [string]$PackagePath,
    [string]$InventoryPath,
    [string]$HashPath,
    [string]$ReleaseVersion,
    [string]$PackageContext
) {
    foreach ($artifact in @(
        @{ path = $PackagePath; name = "Bedrock client package" },
        @{ path = $InventoryPath; name = "Bedrock package inventory" },
        @{ path = $HashPath; name = "Bedrock package SHA-256 record" }
    )) {
        if (-not (Test-Path -LiteralPath $artifact.path -PathType Leaf)) {
            throw "$($artifact.name) is missing or is not a file: $($artifact.path)"
        }
    }

    try {
        $bedrockInventory = Get-Content -LiteralPath $InventoryPath -Raw | ConvertFrom-Json
    }
    catch {
        throw "Bedrock package inventory is invalid JSON: $InventoryPath"
    }
    if ($null -eq $bedrockInventory -or $bedrockInventory -is [System.Array]) {
        throw "Bedrock package inventory must contain one JSON object: $InventoryPath"
    }

    $expectedArchive = "AmalgamBedrockClient-$ReleaseVersion.mcaddon"
    $inventoryVersion = [string]$bedrockInventory.version
    $inventoryPackage = [string]$bedrockInventory.package
    if ($inventoryVersion -ne $ReleaseVersion -or $inventoryPackage -ne $expectedArchive) {
        throw "Bedrock package metadata version mismatch. Expected $ReleaseVersion, found $inventoryVersion"
    }

    $inventoryHash = ([string]$bedrockInventory.sha256).Trim()
    if ($inventoryHash -notmatch '^[0-9a-fA-F]{64}$') {
        throw "Bedrock package inventory SHA-256 is missing or malformed: $InventoryPath"
    }
    $inventoryHash = $inventoryHash.ToLowerInvariant()

    $hashLines = @(Get-Content -LiteralPath $HashPath)
    if ($hashLines.Count -ne 1) {
        throw "Bedrock package SHA-256 record must contain exactly one archive entry: $HashPath"
    }
    if (([string]$hashLines[0]) -notmatch '^(?<hash>[0-9a-fA-F]{64})\s{2,}(?<archive>.+)$') {
        throw "Bedrock package SHA-256 record is malformed: $HashPath"
    }
    if ($Matches['archive'] -cne $expectedArchive) {
        throw "Bedrock package hash metadata version mismatch for $ReleaseVersion"
    }
    $hashRecordHash = $Matches['hash'].ToLowerInvariant()

    $actualHash = (Get-FileHash -LiteralPath $PackagePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($inventoryHash -ne $actualHash) {
        throw "Bedrock package inventory SHA-256 does not match $PackageContext bytes. Rebuild bedrock\\AmalgamBedrockClient with npm run package, then package again."
    }
    if ($hashRecordHash -ne $actualHash) {
        throw "Bedrock package SHA-256 record does not match $PackageContext bytes. Rebuild bedrock\\AmalgamBedrockClient with npm run package, then package again."
    }
}

function Write-ReleaseMetadata([string]$StageDir, [string]$ReleaseVersion) {
    # A previous packaging pass leaves derived files behind. Remove only these
    # exact, known output files before recomputing metadata so the SBOM cannot
    # be self-referential and signatures/hashes cannot describe stale bytes.
    foreach ($derivedName in @("component-manifest.json", "sbom.cdx.json", "release.sha256")) {
        Remove-DerivedReleaseFile (Join-Path $StageDir $derivedName)
    }

    $components = @(
        Get-ChildItem -LiteralPath $StageDir -Recurse -File |
            Where-Object { $_.Name -notin @("component-manifest.json", "sbom.cdx.json", "release.sha256") } |
            Sort-Object FullName |
            ForEach-Object {
                $hash = Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256
                [PSCustomObject]@{
                    name = $_.FullName.Substring($StageDir.Length + 1).Replace("\", "/")
                    sha256 = $hash.Hash.ToLowerInvariant()
                    size = $_.Length
                }
            }
    )
    if ($components.Count -eq 0) {
        throw "Staged release contains no payload files: $StageDir"
    }
    [IO.File]::WriteAllText((Join-Path $StageDir "component-manifest.json"),
        ($components | ConvertTo-Json -Depth 3), $utf8)

    $sbomComponents = @(
        $components | ForEach-Object {
            [PSCustomObject]@{
                type = if ($_.name -like "bridges/*") { "library" } else { "application" }
                name = $_.name
                version = $ReleaseVersion
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
    [IO.File]::WriteAllText((Join-Path $StageDir "sbom.cdx.json"),
        ($sbom | ConvertTo-Json -Depth 8), $utf8)

    $hashLines = @()
    Get-ChildItem -LiteralPath $StageDir -Recurse -File | Sort-Object FullName | ForEach-Object {
        $hash = Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256
        $relative = $_.FullName.Substring($StageDir.Length + 1).Replace("\", "/")
        $hashLines += "$($hash.Hash.ToLowerInvariant())  $relative"
    }
    [IO.File]::WriteAllLines((Join-Path $StageDir "release.sha256"), $hashLines, $utf8)
}

function Write-ReleaseArchive([string]$StageDir, [string]$ArchivePath) {
    Remove-DerivedReleaseFile $ArchivePath
    Compress-Archive -Path (Join-Path $StageDir "*") -DestinationPath $ArchivePath -CompressionLevel Optimal
    if (-not (Test-Path -LiteralPath $ArchivePath -PathType Leaf) -or
        (Get-Item -LiteralPath $ArchivePath).Length -le 0) {
        throw "Final release archive was not created: $ArchivePath"
    }
}

Assert-ReleaseArtifactPath $stage "Release staging directory"
Assert-ReleaseArtifactPath $zip "Release archive"
if ($RequireSignedStagedBinaries -and -not $FinalizeExistingStage) {
    throw "-RequireSignedStagedBinaries is only valid with -FinalizeExistingStage"
}

if ($FinalizeExistingStage) {
    if (-not (Test-Path -LiteralPath $stage -PathType Container)) {
        throw "Existing release staging directory is missing: $stage"
    }
    Assert-StagedNativeBinaries $stage -RequireSignatures:$RequireSignedStagedBinaries
    $stagedBedrockDir = Join-Path $stage "bedrock"
    Assert-BedrockPackageIntegrity `
        -PackagePath (Join-Path $stagedBedrockDir "AmalgamBedrockClient.mcaddon") `
        -InventoryPath (Join-Path $stagedBedrockDir "package-inventory.json") `
        -HashPath (Join-Path $stagedBedrockDir "package-sha256.txt") `
        -ReleaseVersion $Version `
        -PackageContext "staged Bedrock package"
    Write-ReleaseMetadata $stage $Version
    Write-ReleaseArchive $stage $zip
    Write-Output "Finalized staged release after signing: $zip"
    return
}

$BuildDir = (Resolve-Path $BuildDir).Path

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
Assert-BedrockPackageIntegrity `
    -PackagePath $bedrockPackage `
    -InventoryPath $bedrockInventoryPath `
    -HashPath $bedrockHashPath `
    -ReleaseVersion $Version `
    -PackageContext "build Bedrock package"
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
    # CTest considers an empty test registry a successful invocation. A release
    # package must never use that as proof that its native test gate ran.
    $ctestInventory = (& $ctestPath --test-dir $BuildDir -N 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) { throw "CTest could not enumerate tests: $ctestInventory" }
    if ($ctestInventory -notmatch 'Total Tests:\s+([1-9][0-9]*)') {
        throw "CTest found no configured tests in $BuildDir"
    }
    & $ctestPath --test-dir $BuildDir --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "CTest failed" }
}

if (Test-Path -LiteralPath $stage) {
    if (-not (Test-Path -LiteralPath $stage -PathType Container)) {
        throw "Expected a staging directory, found a file: $stage"
    }
    # $stage is version-validated and asserted to be below the requested
    # output root above; no user profile, workspace, or wildcard is removed.
    Remove-Item -LiteralPath $stage -Recurse -Force
}
Remove-DerivedReleaseFile $zip
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
Assert-BedrockPackageIntegrity `
    -PackagePath (Join-Path $stage "bedrock\AmalgamBedrockClient.mcaddon") `
    -InventoryPath (Join-Path $stage "bedrock\package-inventory.json") `
    -HashPath (Join-Path $stage "bedrock\package-sha256.txt") `
    -ReleaseVersion $Version `
    -PackageContext "staged Bedrock package"
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
    website_url = $WebsiteUrl
    api_url = $ApiUrl
    username = ""
    ai_providers = @()
}
$template = $templateData | ConvertTo-Json -Depth 4
[IO.File]::WriteAllText((Join-Path $stage "launcher.json.template"), $template, $utf8)
$prerequisites = @'
{
  "platform": "Windows 10+ x64",
  "launcher_checks": ["tar.exe", "winsqlite3.dll", "VCRUNTIME140.dll", "VCRUNTIME140_1.dll", "MSVCP140.dll"],
  "diagnostics": "SQLite persistence is opt-in, but the Windows 10+ winsqlite3 system component is required by the native DLL"
} 
'@
[IO.File]::WriteAllText((Join-Path $stage "prerequisites.json"), $prerequisites, $utf8)
Write-ReleaseMetadata $stage $Version
Write-ReleaseArchive $stage $zip
Write-Output "Packaged $zip"
