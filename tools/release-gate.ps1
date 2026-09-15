param(
    [string]$Version = "1.0.0",
    [string]$BuildDir = "",
    [switch]$RequireSigned,
    [switch]$AllowInertConfig,
    [switch]$SkipBuild
)

# One-command official release gate. Chains every locally runnable check in
# release order and stops at the first failure. External gates (code-signing
# certificate, clean-machine matrix, live services) are reported at the end.
#
# The gate fails closed when the package would ship without its public backend
# and sign-in configuration. -AllowInertConfig produces a local test package and
# marks the result NOT RELEASABLE.
$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
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

try {
    # 1. Build toolchain and release vault sanity.
    Step "toolchain"
    if (-not (Test-Path (Join-Path $BuildDir "CMakeCache.txt")) -and -not $SkipBuild) {
        throw "BuildDir has no CMakeCache.txt; run tools\build-release.cmd first or omit -SkipBuild"
    }
    $vaultKey = "C:\Users\David\.amalgam-release\update-signing-public.pem"
    if (Test-Path $vaultKey) {
        Pass "release vault present ($vaultKey)"
    } else {
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
    if ($LASTEXITCODE -ne 0) { throw "native tests failed" }
    Pass "native tests"

    # 4. Package release (runs CTest as a hard gate; honors public config).
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
    if ($LASTEXITCODE -ne 0) { throw "packaging failed" }
    Pass "release package (CTest included)"

    # 5. Installer.
    Step "installer"
    & (Join-Path $root "tools\build-installer.ps1") -SourceDir $packageDir -Version $Version
    if ($LASTEXITCODE -ne 0) { throw "installer build failed" }
    Pass "installer compiled"

    $templatePath = Join-Path $packageDir "launcher.json.template"
    if (Test-Path -LiteralPath $templatePath) {
        $templateConfig = Get-Content -LiteralPath $templatePath -Raw | ConvertFrom-Json
        $packagePublicConfig =
            -not [string]::IsNullOrWhiteSpace([string]$templateConfig.microsoft_client_id) -and
            -not [string]::IsNullOrWhiteSpace([string]$templateConfig.supabase_url) -and
            -not [string]::IsNullOrWhiteSpace([string]$templateConfig.supabase_anon_key)
    }

    # 6. Package integrity (hashes, manifest, SBOM).
    Step "package-validation"
    & (Join-Path $root "tools\validate-package.ps1") -Package $zipPath -RequireOnlineConfig:$requirePublicConfig
    if ($LASTEXITCODE -ne 0) { throw "package validation failed" }
    Pass "package integrity"

    # 7. Clean extracted-package runtime checks.
    Step "runtime-validation"
    & (Join-Path $root "tools\validate-package-runtime.ps1") -Package $zipPath
    if ($LASTEXITCODE -ne 0) { throw "runtime validation failed" }
    Pass "extracted-package runtime"

    # 8. Installer staging inputs.
    Step "installer-inputs"
    & (Join-Path $root "tools\qa-installer-inputs.ps1") -SourceDir $packageDir
    if ($LASTEXITCODE -ne 0) { throw "installer inputs failed" }
    Pass "installer staging inputs"

    # 9. Cached launch matrix (Fabric/Quilt/NeoForge/Forge dry runs).
    Step "launch-matrix"
    & (Join-Path $root "tools\validate-cached-launches.ps1") -BuildDir $BuildDir
    if ($LASTEXITCODE -ne 0) { throw "launch matrix failed" }
    Pass "cached launch matrix"

    # 10. Packaged launcher smoke.
    Step "launcher-smoke"
    & (Join-Path $root "tools\qa-launcher-smoke.ps1") -PackageDir $packageDir
    if ($LASTEXITCODE -ne 0) { throw "launcher smoke failed" }
    Pass "launcher smoke"

    # 11. Node suites.
    Step "node-suites"
    & npm test --prefix (Join-Path $root "tools\runtime-agent")
    if ($LASTEXITCODE -ne 0) { throw "runtime-agent suite failed" }
    $nodeTests = (Get-ChildItem (Join-Path $root "tools") -Filter "*.test.mjs").FullName
    & node --test @nodeTests
    if ($LASTEXITCODE -ne 0) { throw "tooling suite failed" }
    Pass "node suites"

    # 12. Secret scan.
    Step "secret-scan"
    & node (Join-Path $root "tools\secret-scan.mjs") tools installer (Join-Path $root "cpp\launcher")
    if ($LASTEXITCODE -ne 0) { throw "secret scan reported findings" }
    Pass "secret scan"

    # 13. Updater verification key embedded and matching the vault.
    Step "updater-key"
    $generatedHeader = Join-Path $BuildDir "generated\updater_public_key.h"
    if ((Test-Path $vaultKey) -and (Test-Path $generatedHeader)) {
        $vaultPem = (Get-Content $vaultKey -Raw).Replace("`r`n", "`n").Trim()
        $headerText = Get-Content $generatedHeader -Raw
        if ($headerText -match [regex]::Escape($vaultPem)) {
            Pass "embedded updater key matches vault public key"
        } else {
            throw "embedded updater key differs from vault public key; reconfigure with -DAMALGAM_UPDATER_PUBLIC_KEY_FILE"
        }
    } else {
        Write-Warning "updater key not embedded (fail-closed updates only)"
    }

    # 14. Authenticode signing (advisory unless -RequireSigned).
    Step "code-signing"
    & (Join-Path $root "tools\verify-signing.ps1") -PackageDir $packageDir -InstallerPath $installerPath -RequireSigned:$RequireSigned
    if ($LASTEXITCODE -ne 0) { throw "signing verification failed" }
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
Write-Output "RESULT: ALL LOCAL GATES PASSED for Amalgam $Version"
Write-Output ""
if (-not $packagePublicConfig) {
    Write-Output "NOT RELEASABLE: package was built without a required public configuration"
    Write-Output "  (rerun without -AllowInertConfig and with AMALGAM_MICROSOFT_CLIENT_ID,"
    Write-Output "   AMALGAM_SUPABASE_URL, and AMALGAM_SUPABASE_PUBLISHABLE_KEY set)"
}
Write-Output "External gates that remain (cannot be verified in this tree):"
Write-Output "  - Authenticode certificate for launcher, DLL, and installer"
if (-not $RequireSigned) { Write-Output "    (rerun with -RequireSigned once binaries are signed)" }
Write-Output "  - Clean-machine install / uninstall / upgrade matrix"
Write-Output "  - Publisher-approved Microsoft/Minecraft account sign-in"
Write-Output "  - CurseForge production key approval"
Write-Output "  - Deployed update feed at the manifest endpoint"
exit 0
