param(
    [string]$Version = "1.0.0",
    # Subject substring (CN=...) of the code-signing certificate in the user
    # or machine store, or a PFX path with -PfxPassword.
    [string]$Subject,
    [string]$PfxPath,
    [string]$PfxPassword,
    [string]$TimestampUrl = "http://timestamp.digicert.com",
    # sha256 is the modern default; sha1 only for legacy targets.
    [string]$DigestAlg = "sha256",
    [switch]$WhatIf
)

# Signs the three release artifacts with Authenticode and verifies the result.
# Without a certificate this script documents exactly what is missing; run it
# once the release certificate is installed on the signing machine.
$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$launcher = Join-Path $root "dist\amalgam-$Version\amalgam_launcher.exe"
$dll = Join-Path $root "dist\amalgam-$Version\amalgam.dll"
$installer = Join-Path $root "dist\installer\AmalgamLauncher-$Version-Setup.exe"
$artifacts = @($launcher, $dll, $installer)
foreach ($a in $artifacts) {
    if (-not (Test-Path $a)) { throw "Missing artifact: $a" }
}

$signtool = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin\*\x64\signtool.exe",
                        "C:\Program Files (x86)\Windows Kits\10\bin\*\x86\signtool.exe" `
                   -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending | Select-Object -First 1
if (-not $signtool) { throw "signtool.exe not found; install the Windows SDK" }
Write-Output "signtool: $($signtool.FullName)"

if ($PfxPath) {
    if (-not $PfxPassword) { throw "-PfxPassword is required with -PfxPath" }
    $certArgs = @("/f", $PfxPath, "/p", $PfxPassword)
    Write-Output "signing with PFX: $PfxPath"
} elseif ($Subject) {
    $certArgs = @("/n", $Subject)
    Write-Output "signing with store certificate matching: $Subject"
} else {
    Write-Output ""
    Write-Output "NO CERTIFICATE CONFIGURED. To complete the public-release gate:"
    Write-Output "  1. Obtain an OV/EV code-signing certificate from your CA."
    Write-Output "  2. Install it: Certutil -user -f PFX -p <password> <file>.pfx  (or import to the machine store)."
    Write-Output "  3. Re-run:  tools\sign-release.ps1 -Version $Version -Subject '<cert CN>'"
    Write-Output "             (or -PfxPath <file>.pfx -PfxPassword <password>)"
    Write-Output "  4. Re-run tools\release-gate.ps1 -Version $Version -RequireSigned"
    Write-Output ""
    throw "no signing identity given (-Subject or -PfxPath)"
}

$common = @("sign", "/fd", $DigestAlg, "/td", "sha256", "/tr", $TimestampUrl) + $certArgs
foreach ($a in $artifacts) {
    Write-Output "signing $(Split-Path -Leaf $a)..."
    if ($WhatIf) { Write-Output "(what-if) signtool $($common -join ' ') `"$a`""; continue }
    & $signtool.FullName @common $a
    if ($LASTEXITCODE -ne 0) { throw "signtool failed for $a (exit $LASTEXITCODE)" }
}

Write-Output ""
Write-Output "verifying signatures..."
$unsigned = @()
foreach ($a in $artifacts) {
    $sig = Get-AuthenticodeSignature -LiteralPath $a
    if ($sig.Status -eq "Valid") {
        Write-Output "OK  $(Split-Path -Leaf $a)  signed by $($sig.SignerCertificate.Subject)"
    } else {
        $unsigned += "$(Split-Path -Leaf $a): $($sig.Status)"
    }
}
if ($unsigned.Count -gt 0) { throw "signature verification failed: $($unsigned -join '; ')" }
Write-Output ""
Write-Output "All artifacts signed and verified. Next: tools\release-gate.ps1 -Version $Version -RequireSigned"
