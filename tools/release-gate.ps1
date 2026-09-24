param(
    [string]$Version = "1.0.0",
    [string]$BuildDir = "",
    [switch]$RequireSigned,
    [switch]$AllowInertConfig,
    # Explicitly permits a non-releasable local candidate without generating
    # and verifying a signed final update payload.
    [switch]$AllowInertUpdates,
    [string]$SigningSubject,
    [string]$SigningPfxPath,
    [string]$SigningPfxPassword,
    [string]$TimestampUrl = "http://timestamp.digicert.com",
    [string]$UpdatePublicKeyPath = "C:\Users\David\.amalgam-release\update-signing-public.pem",
    [string]$UpdatePrivateKeyPath,
    [string]$UpdatePayloadUrl,
    [string]$UpdateChannel = "stable",
    [string]$UpdateMinVersion = "",
    [string]$UpdateNotes = "",
    [switch]$UpdateMandatory,
    [switch]$SkipBuild
)

# One-command official release gate. Chains every locally runnable check in
# release order and stops at the first failure. A public invocation signs the
# stage before finalization, signs the installer after it is built, then creates
# and verifies an update manifest against the final ZIP. External deployment,
# clean-machine, and live-service gates are reported at the end.
#
# The gate fails closed when the package would ship without its public backend
# and sign-in configuration or a cryptographically verifiable update payload.
# -AllowInertConfig and -AllowInertUpdates are deliberate local-test escapes;
# either one marks the result NOT RELEASABLE and cannot be combined with
# -RequireSigned.
$ErrorActionPreference = "Stop"
if ($Version -notmatch '^[0-9A-Za-z][0-9A-Za-z._-]*$') {
    throw "Version may contain only letters, digits, dots, underscores, and hyphens"
}
if ($RequireSigned -and ($AllowInertConfig -or $AllowInertUpdates)) {
    throw "-RequireSigned cannot be combined with -AllowInertConfig or -AllowInertUpdates"
}
if (-not $RequireSigned -and -not $AllowInertUpdates) {
    throw "A release without signed update verification is non-releasable; pass -AllowInertUpdates only for a local test candidate, or use -RequireSigned with signing/update credentials"
}
if ($RequireSigned) {
    if (([string]::IsNullOrWhiteSpace($SigningSubject) -and [string]::IsNullOrWhiteSpace($SigningPfxPath)) -or
        (-not [string]::IsNullOrWhiteSpace($SigningSubject) -and -not [string]::IsNullOrWhiteSpace($SigningPfxPath))) {
        throw "-RequireSigned needs exactly one signing identity: -SigningSubject or -SigningPfxPath"
    }
    if (-not [string]::IsNullOrWhiteSpace($SigningPfxPath) -and [string]::IsNullOrEmpty($SigningPfxPassword)) {
        throw "-SigningPfxPassword is required with -SigningPfxPath"
    }
    if ([string]::IsNullOrWhiteSpace($UpdatePrivateKeyPath) -or [string]::IsNullOrWhiteSpace($UpdatePayloadUrl)) {
        throw "-RequireSigned needs -UpdatePrivateKeyPath and an explicit -UpdatePayloadUrl for the final ZIP"
    }
    if ($UpdatePayloadUrl -notmatch '^https://') {
        throw "-UpdatePayloadUrl must be an https URL"
    }
}
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if ($RequireSigned) {
    # Validate file-backed signing/update inputs before a build or signing step
    # can mutate a staging directory. Certificate-store subjects are resolved
    # by signtool at signing time; file paths can be checked safely here.
    if (-not (Test-Path -LiteralPath $UpdatePrivateKeyPath -PathType Leaf)) {
        throw "Production update private key is missing: $UpdatePrivateKeyPath"
    }
    $UpdatePrivateKeyPath = (Resolve-Path -LiteralPath $UpdatePrivateKeyPath).Path
    if (-not [string]::IsNullOrWhiteSpace($SigningPfxPath)) {
        if (-not (Test-Path -LiteralPath $SigningPfxPath -PathType Leaf)) {
            throw "Signing PFX is missing: $SigningPfxPath"
        }
        $SigningPfxPath = (Resolve-Path -LiteralPath $SigningPfxPath).Path
    }
}
if (Test-Path -LiteralPath $UpdatePublicKeyPath -PathType Leaf) {
    $UpdatePublicKeyPath = (Resolve-Path -LiteralPath $UpdatePublicKeyPath).Path
}
if (-not $BuildDir) { $BuildDir = Join-Path $root "cpp\build-release" }
$packageDir = Join-Path $root "dist\amalgam-$Version"
$zipPath = Join-Path $root "dist\amalgam-$Version.zip"
$installerPath = Join-Path $root "dist\installer\AmalgamLauncher-$Version-Setup.exe"

function Step([string]$name) { Write-Output "" ; Write-Output "=== GATE: $name ===" }
function Pass([string]$name) { Write-Output "[PASS] $name" }
$failed = $false
# Whether the staged package actually carries its public backend and sign-in
# configuration. The summary reports this, not the flag, so the result is the
# truth about the artifact whichever way the gate was invoked.
$packagePublicConfig = $false
$finalUpdateVerified = $false

try {
    # 1. Build toolchain and release vault sanity.
    Step "toolchain"
    if (-not (Test-Path (Join-Path $BuildDir "CMakeCache.txt")) -and -not $SkipBuild) {
        throw "BuildDir has no CMakeCache.txt; run tools\build-release.cmd first or omit -SkipBuild"
    }
    $vaultKey = $UpdatePublicKeyPath
    if (Test-Path -LiteralPath $vaultKey -PathType Leaf) {
        Pass "release vault present ($vaultKey)"
    } else {
        if ($RequireSigned) {
            throw "Production update public key is missing: $vaultKey"
        }
        Write-Warning "release vault missing: signed update manifests will fail closed until AMALGAM_UPDATER_PUBLIC_KEY_FILE is configured"
    }

    # 2. Clean release build (also stages bridges and the Bedrock package).
    if (-not $SkipBuild) {
        Step "build"
        & (Join-Path $root "tools\build-release.cmd")
        if ($LASTEXITCODE -ne 0) { throw "build-release.cmd failed" }
        Pass "clean release build"
    }

    # 3. Native unit tests on the fresh binaries.
    Step "native-tests"
    & (Join-Path $root "tools\qa-native-run.ps1") -BuildDir $BuildDir
    if (-not $?) { throw "native tests failed" }
    Pass "native tests"

    # 4. Create the initial staging directory. For public releases this is
    # deliberately unsigned only until the next phase signs the staged native
    # binaries; no installer is built from this initial stage.
    Step "package"
    # An official package must carry the public backend and sign-in configuration.
    # Without it the shipped launcher is inert, so the gate fails closed unless a
    # developer explicitly asks for a configuration-free test package.
    $requirePublicConfig = -not $AllowInertConfig
    $pkgArgs = @{
        BuildDir  = $BuildDir
        OutputDir = (Join-Path $root "dist")
        Version   = $Version
    }
    if ($requirePublicConfig) { $pkgArgs.RequireOnlineConfig = $true }
    & (Join-Path $root "tools\package-release.ps1") @pkgArgs
    if (-not $?) { throw "packaging failed" }
    Pass "release package (CTest included)"

    if ($RequireSigned) {
        # Signing changes PE bytes. Finalize the stage immediately afterward so
        # component-manifest.json, SBOM, release.sha256, and the ZIP bind the
        # signed files rather than their old unsigned copies.
        Step "stage-signing"
        $stageSignArgs = @{
            Version = $Version
            StageDir = $packageDir
            TimestampUrl = $TimestampUrl
            StageBinariesOnly = $true
        }
        if (-not [string]::IsNullOrWhiteSpace($SigningPfxPath)) {
            $stageSignArgs.PfxPath = $SigningPfxPath
            $stageSignArgs.PfxPassword = $SigningPfxPassword
        } else {
            $stageSignArgs.Subject = $SigningSubject
        }
        & (Join-Path $root "tools\sign-release.ps1") @stageSignArgs
        if (-not $?) { throw "staged binary signing failed" }
        Pass "staged launcher and DLL signed"

        Step "finalize-signed-stage"
        & (Join-Path $root "tools\package-release.ps1") -OutputDir (Join-Path $root "dist") -Version $Version `
            -FinalizeExistingStage -RequireSignedStagedBinaries
        if (-not $?) { throw "signed-stage finalization failed" }
        Pass "signed stage hashes, SBOM, and ZIP regenerated"
    }

    # 5. Validate the exact stage that will be embedded into the installer.
    Step "installer-inputs"
    & (Join-Path $root "tools\qa-installer-inputs.ps1") -SourceDir $packageDir
    if (-not $?) { throw "installer inputs failed" }
    Pass "installer staging inputs"

    # 6. Build the installer only after staged signatures and package metadata
    # are final, so its embedded native files are the signed ones.
    Step "installer"
    & (Join-Path $root "tools\build-installer.ps1") -SourceDir $packageDir -Version $Version
    if (-not $?) { throw "installer build failed" }
    Pass "installer compiled"

    if ($RequireSigned) {
        Step "installer-signing"
        $installerSignArgs = @{
            Version = $Version
            InstallerPath = $installerPath
            TimestampUrl = $TimestampUrl
            InstallerOnly = $true
        }
        if (-not [string]::IsNullOrWhiteSpace($SigningPfxPath)) {
            $installerSignArgs.PfxPath = $SigningPfxPath
            $installerSignArgs.PfxPassword = $SigningPfxPassword
        } else {
            $installerSignArgs.Subject = $SigningSubject
        }
        & (Join-Path $root "tools\sign-release.ps1") @installerSignArgs
        if (-not $?) { throw "installer signing failed" }
        Pass "installer signed"
    }

    $templatePath = Join-Path $packageDir "launcher.json.template"
    if (Test-Path -LiteralPath $templatePath) {
        $templateConfig = Get-Content -LiteralPath $templatePath -Raw | ConvertFrom-Json
        $packagePublicConfig =
            -not [string]::IsNullOrWhiteSpace([string]$templateConfig.microsoft_client_id) -and
            -not [string]::IsNullOrWhiteSpace([string]$templateConfig.supabase_url) -and
            -not [string]::IsNullOrWhiteSpace([string]$templateConfig.supabase_anon_key)
    }

    # 7. Package integrity (hashes, manifest, SBOM).
    Step "package-validation"
    & (Join-Path $root "tools\validate-package.ps1") -Package $zipPath -RequireOnlineConfig:$requirePublicConfig
    if (-not $?) { throw "package validation failed" }
    Pass "package integrity"

    # 8. Clean extracted-package runtime checks.
    Step "runtime-validation"
    & (Join-Path $root "tools\validate-package-runtime.ps1") -Package $zipPath
    if (-not $?) { throw "runtime validation failed" }
    Pass "extracted-package runtime"

    # 9. Cached launch matrix (Fabric/Quilt/NeoForge/Forge dry runs).
    Step "launch-matrix"
    & (Join-Path $root "tools\validate-cached-launches.ps1") -BuildDir $BuildDir
    if (-not $?) { throw "launch matrix failed" }
    Pass "cached launch matrix"

    # 10. Packaged launcher smoke.
    Step "launcher-smoke"
    & (Join-Path $root "tools\qa-launcher-smoke.ps1") -PackageDir $packageDir
    if (-not $?) { throw "launcher smoke failed" }
    Pass "launcher smoke"

    # 11. Node suites.
    Step "node-suites"
    & npm test --prefix (Join-Path $root "tools\runtime-agent")
    if ($LASTEXITCODE -ne 0) { throw "runtime-agent suite failed" }
    $nodeTests = @(Get-ChildItem -LiteralPath (Join-Path $root "tools") -Filter "*.test.mjs" -File |
        Sort-Object Name | ForEach-Object { $_.FullName })
    if ($nodeTests.Count -eq 0) { throw "No Node tooling tests were found" }
    & node --test @nodeTests
    if ($LASTEXITCODE -ne 0) { throw "tooling suite failed" }
    Pass "node suites"

    # 12. Secret scan.
    Step "secret-scan"
    & node (Join-Path $root "tools\secret-scan.mjs") (Join-Path $root "tools") (Join-Path $root "installer") (Join-Path $root "cpp\launcher")
    if ($LASTEXITCODE -ne 0) { throw "secret scan reported findings" }
    Pass "secret scan"

    # 13. Updater verification key embedded and matching the vault.
    Step "updater-key"
    $generatedHeader = Join-Path $BuildDir "generated\updater_public_key.h"
    if ((Test-Path -LiteralPath $vaultKey -PathType Leaf) -and (Test-Path -LiteralPath $generatedHeader -PathType Leaf)) {
        $vaultPem = (Get-Content -LiteralPath $vaultKey -Raw).Replace("`r`n", "`n").Trim()
        $headerText = Get-Content $generatedHeader -Raw
        if ($headerText -match [regex]::Escape($vaultPem)) {
            Pass "embedded updater key matches vault public key"
        } else {
            throw "embedded updater key differs from vault public key; reconfigure with -DAMALGAM_UPDATER_PUBLIC_KEY_FILE"
        }
    } else {
        if ($RequireSigned) {
            throw "Production update verification requires both the vault public key and generated updater header"
        }
        Write-Warning "updater key not embedded (fail-closed updates only)"
    }

    # 14. Authenticode signatures and the finalized-stage binding.
    Step "code-signing"
    $verifySigningArgs = @{
        PackageDir = $packageDir
        InstallerPath = $installerPath
        RequireSigned = $RequireSigned
    }
    if ($RequireSigned) {
        $verifySigningArgs.RequireSameSigner = $true
        $verifySigningArgs.RequireComponentManifest = $true
    }
    & (Join-Path $root "tools\verify-signing.ps1") @verifySigningArgs
    if (-not $?) { throw "signing verification failed" }
    Pass "code-signing verification"

    # 15. A production manifest is generated only from the final ZIP and is
    # immediately checked against both that file and the public key embedded in
    # the launcher. The gate intentionally does not upload anything.
    Step "update-feed"
    if ($RequireSigned) {
        if (-not (Test-Path -LiteralPath $UpdatePrivateKeyPath -PathType Leaf)) {
            throw "Update private key is missing: $UpdatePrivateKeyPath"
        }
        if ([string]::IsNullOrWhiteSpace($UpdateChannel)) {
            throw "-UpdateChannel is required for a production update manifest"
        }
        $feedDir = Join-Path $root "dist\update-feed"
        New-Item -ItemType Directory -Path $feedDir -Force | Out-Null
        $manifestPath = Join-Path $feedDir "manifest.json"
        $generateArgs = @(
            "generate", "--version", $Version,
            "--channel", $UpdateChannel,
            "--url", $UpdatePayloadUrl,
            "--file", $zipPath,
            "--min-version", $UpdateMinVersion,
            "--notes", $UpdateNotes,
            "--private-key", $UpdatePrivateKeyPath,
            "--out", $manifestPath
        )
        if ($UpdateMandatory) { $generateArgs += "--mandatory" }
        & node (Join-Path $root "tools\update-manifest.mjs") @generateArgs
        if ($LASTEXITCODE -ne 0) { throw "update manifest generation failed" }
        & node (Join-Path $root "tools\update-manifest.mjs") "sign-payload" "--manifest" $manifestPath `
            "--file" $zipPath "--private-key" $UpdatePrivateKeyPath
        if ($LASTEXITCODE -ne 0) { throw "update payload signing failed" }
        & node (Join-Path $root "tools\update-manifest.mjs") "verify" "--manifest" $manifestPath `
            "--public-key" $vaultKey "--payload" $zipPath "--require-payload-signature"
        if ($LASTEXITCODE -ne 0) { throw "final update manifest or payload verification failed" }
        $finalUpdateVerified = $true
        Pass "final update manifest, payload hash, size, and signatures"
    } else {
        Write-Warning "Update payload signing was intentionally skipped for this local test candidate (-AllowInertUpdates)"
    }
} catch {
    Write-Output ""
    Write-Output "[FAIL] $($_.Exception.Message)"
    $failed = $true
}

Write-Output ""
Write-Output "==================== RELEASE GATE SUMMARY ===================="
if ($failed) {
    Write-Output "RESULT: FAILED - fix the gate above before release"
    exit 1
}
if ($RequireSigned -and $packagePublicConfig -and $finalUpdateVerified) {
    Write-Output "RESULT: PRODUCTION LOCAL GATES PASSED for Amalgam $Version"
} else {
    Write-Output "RESULT: LOCAL TEST GATES PASSED - NOT RELEASABLE"
}
Write-Output ""
if (-not $packagePublicConfig) {
    Write-Output "NOT RELEASABLE: package was built without a required public configuration"
    Write-Output "  (rerun without -AllowInertConfig and with AMALGAM_MICROSOFT_CLIENT_ID,"
    Write-Output "   AMALGAM_SUPABASE_URL, and AMALGAM_SUPABASE_PUBLISHABLE_KEY set)"
}
if (-not $finalUpdateVerified) {
    Write-Output "NOT RELEASABLE: no final ZIP update payload was signed and verified"
    Write-Output "  (use -RequireSigned with explicit signing/update inputs; -AllowInertUpdates is local-test only)"
}
Write-Output "External gates that remain (cannot be verified in this tree):"
if (-not $RequireSigned) {
    Write-Output "  - Authenticode certificate for launcher, DLL, and installer"
    Write-Output "    (run the two signing phases through -RequireSigned with a configured identity)"
}
Write-Output "  - Clean-machine install / uninstall / upgrade matrix"
Write-Output "  - Publisher-approved Microsoft/Minecraft account sign-in"
Write-Output "  - CurseForge production key approval"
Write-Output "  - Deployed update feed: upload the locally verified manifest and final ZIP, then fetch and compare them verbatim from the publisher-approved endpoint"
exit 0
