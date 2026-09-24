param(
    [string]$Version = "1.0.0",
    # Subject substring (CN=...) of the code-signing certificate in the user
    # or machine store, or a PFX path with -PfxPassword.
    [string]$Subject,
    [string]$PfxPath,
    [string]$PfxPassword,
    [string]$TimestampUrl = "http://timestamp.digicert.com",
    # sha256 is the modern default; sha1 only for legacy targets.
    [ValidateSet("sha256", "sha1")]
    [string]$DigestAlg = "sha256",
    # The signing order is deliberate. Stage binaries first, regenerate the
    # package metadata/ZIP, build the installer from that signed stage, then
    # sign the installer. A one-shot "sign everything" mode is intentionally
    # not offered because it produces stale package hashes.
    [switch]$StageBinariesOnly,
    [switch]$InstallerOnly,
    [string]$StageDir = "",
    [string]$InstallerPath = "",
    [switch]$WhatIf
)

# Signs exactly one release phase and verifies it. Private keys and PFX
# passwords are never echoed. See docs/release-gates.md for the required
# production sequence.
$ErrorActionPreference = "Stop"
if ($Version -notmatch '^[0-9A-Za-z][0-9A-Za-z._-]*$') {
    throw "Version may contain only letters, digits, dots, underscores, and hyphens"
}
if (($StageBinariesOnly -and $InstallerOnly) -or
    (-not $StageBinariesOnly -and -not $InstallerOnly)) {
    throw "Choose exactly one signing phase: -StageBinariesOnly or -InstallerOnly"
}

$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if ([string]::IsNullOrWhiteSpace($StageDir)) {
    $StageDir = Join-Path $root "dist\amalgam-$Version"
}
if ([string]::IsNullOrWhiteSpace($InstallerPath)) {
    $InstallerPath = Join-Path $root "dist\installer\AmalgamLauncher-$Version-Setup.exe"
}

if ($StageBinariesOnly) {
    $StageDir = (Resolve-Path -LiteralPath $StageDir).Path
    $artifacts = @(
        (Join-Path $StageDir "amalgam_launcher.exe"),
        (Join-Path $StageDir "amalgam.dll")
    )
    $phase = "staged launcher and DLL"
} else {
    $artifacts = @([IO.Path]::GetFullPath($InstallerPath))
    $phase = "installer"
}
foreach ($artifact in $artifacts) {
    if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
        throw "Missing $phase artifact: $artifact"
    }
}

if ($PfxPath) {
    if (-not (Test-Path -LiteralPath $PfxPath -PathType Leaf)) {
        throw "PFX file is missing: $PfxPath"
    }
    if ([string]::IsNullOrEmpty($PfxPassword)) {
        throw "-PfxPassword is required with -PfxPath"
    }
    $certArgs = @("/f", $PfxPath, "/p", $PfxPassword)
    $displayCertArgs = @("/f", $PfxPath, "/p", "<redacted>")
    Write-Output "signing $phase with PFX: $PfxPath"
} elseif ($Subject) {
    $certArgs = @("/n", $Subject)
    $displayCertArgs = $certArgs
    Write-Output "signing $phase with store certificate matching: $Subject"
} else {
    Write-Output ""
    Write-Output "NO CERTIFICATE CONFIGURED. A release is signed in two phases:"
    Write-Output "  1. sign-release.ps1 -StageBinariesOnly (launcher and DLL)"
    Write-Output "  2. package-release.ps1 -FinalizeExistingStage -RequireSignedStagedBinaries"
    Write-Output "  3. build-installer.ps1 from that finalized stage"
    Write-Output "  4. sign-release.ps1 -InstallerOnly"
    Write-Output ""
    throw "no signing identity given (-Subject or -PfxPath)"
}

$common = @("sign", "/fd", $DigestAlg, "/td", "sha256", "/tr", $TimestampUrl) + $certArgs
$displayCommon = @("sign", "/fd", $DigestAlg, "/td", "sha256", "/tr", $TimestampUrl) + $displayCertArgs
if ($WhatIf) {
    foreach ($artifact in $artifacts) {
        Write-Output "(what-if) signtool $($displayCommon -join ' ') `"$artifact`""
    }
    Write-Output "No artifact was modified."
    return
}

$signtool = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin\*\x64\signtool.exe",
                        "C:\Program Files (x86)\Windows Kits\10\bin\*\x86\signtool.exe" `
                   -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending | Select-Object -First 1
if (-not $signtool) { throw "signtool.exe not found; install the Windows SDK" }
Write-Output "signtool: $($signtool.FullName)"

foreach ($artifact in $artifacts) {
    Write-Output "signing $(Split-Path -Leaf $artifact)..."
    & $signtool.FullName @common $artifact
    if ($LASTEXITCODE -ne 0) { throw "signtool failed for $artifact (exit $LASTEXITCODE)" }
}

Write-Output ""
Write-Output "verifying $phase signatures..."
$invalid = @()
foreach ($artifact in $artifacts) {
    $signature = Get-AuthenticodeSignature -LiteralPath $artifact
    if ($signature.Status -eq "Valid") {
        Write-Output "OK  $(Split-Path -Leaf $artifact)  signed by $($signature.SignerCertificate.Subject)"
    } else {
        $invalid += "$(Split-Path -Leaf $artifact): $($signature.Status)"
    }
}
if ($invalid.Count -gt 0) { throw "signature verification failed: $($invalid -join '; ')" }

Write-Output ""
if ($StageBinariesOnly) {
    Write-Output "Staged binaries signed and verified. Next: tools\package-release.ps1 -Version $Version -FinalizeExistingStage -RequireSignedStagedBinaries"
} else {
    Write-Output "Installer signed and verified. Next: tools\verify-signing.ps1 -RequireSigned -RequireSameSigner -RequireComponentManifest"
}
