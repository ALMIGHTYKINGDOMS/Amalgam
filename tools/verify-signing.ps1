param(
    [string]$PackageDir,
    [string]$InstallerPath,
    [switch]$RequireSigned
)

$ErrorActionPreference = "Stop"
if (-not $PackageDir) { throw "PackageDir is required" }
$files = @(
    (Join-Path $PackageDir "amalgam_launcher.exe"),
    (Join-Path $PackageDir "amalgam.dll")
)
if ($InstallerPath) { $files += $InstallerPath }
$unsigned = @()
foreach ($path in $files) {
    if (-not (Test-Path -LiteralPath $path)) { throw "Missing artifact: $path" }
    $signature = Get-AuthenticodeSignature -LiteralPath $path
    $name = Split-Path -Leaf $path
    if ($signature.Status -ne "Valid") {
        $unsigned += $name
        Write-Warning "$name is not validly signed ($($signature.Status))"
    } else {
        Write-Output "$name signed by $($signature.SignerCertificate.Subject)"
    }
}
if ($RequireSigned -and $unsigned.Count -gt 0) { throw "Unsigned release artifacts: $($unsigned -join ', ')" }
if ($unsigned.Count -eq 0) { Write-Output "Signing verification passed" }
else { Write-Output "Signing verification is advisory; provide a release certificate before public distribution" }
