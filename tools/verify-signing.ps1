param(
    [string]$PackageDir,
    [string]$InstallerPath,
    [switch]$RequireSigned,
    # Production artifacts should be signed by the same publishing identity.
    [switch]$RequireSameSigner,
    # Confirm the finalized component manifest binds the actual staged EXE and
    # DLL bytes, catching the stale-hash case caused by signing after packaging.
    [switch]$RequireComponentManifest
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($PackageDir)) { throw "PackageDir is required" }
$PackageDir = (Resolve-Path -LiteralPath $PackageDir).Path

function Assert-ComponentManifestBindings([string]$StageDir) {
    $manifestPath = Join-Path $StageDir "component-manifest.json"
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "Final component manifest is missing: $manifestPath"
    }
    try {
        # Windows PowerShell returns a JSON array as one Object[] value here.
        # Keep it intact so foreach enumerates its component objects instead of
        # wrapping the entire manifest in a one-item outer array.
        $components = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    } catch {
        throw "Final component manifest is malformed: $($_.Exception.Message)"
    }
    foreach ($name in @("amalgam_launcher.exe", "amalgam.dll")) {
        # Do not call this $matches: PowerShell's case-insensitive $Matches
        # automatic variable is overwritten by the regex check below. Use an
        # explicit foreach instead of a pipeline so Windows PowerShell also
        # handles a JSON Object[] manifest correctly.
        $componentEntries = @()
        foreach ($component in $components) {
            if ($component.name -eq $name) { $componentEntries += $component }
        }
        if ($componentEntries.Count -ne 1) {
            throw "Final component manifest must contain exactly one entry for $name"
        }
        $artifact = Join-Path $StageDir $name
        if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
            throw "Staged component is missing: $artifact"
        }
        $expectedHash = [string]$componentEntries[0].sha256
        if ($expectedHash -notmatch '^[0-9a-fA-F]{64}$') {
            throw "Final component manifest hash is malformed for $name"
        }
        $actualHash = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash
        if ($actualHash -ine $expectedHash) {
            throw "Final component manifest hash does not match $name; finalize the signed stage before building the installer"
        }
        $expectedSize = [int64]$componentEntries[0].size
        $actualSize = (Get-Item -LiteralPath $artifact).Length
        if ($actualSize -ne $expectedSize) {
            throw "Final component manifest size does not match $name (manifest=$expectedSize actual=$actualSize); finalize the signed stage before building the installer"
        }
    }
    Write-Output "Component manifest binds the current staged native artifacts"
}

$files = @(
    [PSCustomObject]@{ Path = (Join-Path $PackageDir "amalgam_launcher.exe"); Name = "amalgam_launcher.exe" },
    [PSCustomObject]@{ Path = (Join-Path $PackageDir "amalgam.dll"); Name = "amalgam.dll" }
)
if (-not [string]::IsNullOrWhiteSpace($InstallerPath)) {
    $files += [PSCustomObject]@{ Path = [IO.Path]::GetFullPath($InstallerPath); Name = (Split-Path -Leaf $InstallerPath) }
}

if ($RequireComponentManifest) {
    Assert-ComponentManifestBindings $PackageDir
}

$invalid = @()
$signerThumbprints = @()
foreach ($file in $files) {
    if (-not (Test-Path -LiteralPath $file.Path -PathType Leaf)) {
        throw "Missing artifact: $($file.Path)"
    }
    $signature = Get-AuthenticodeSignature -LiteralPath $file.Path
    if ($signature.Status -ne "Valid" -or -not $signature.SignerCertificate) {
        $invalid += $file.Name
        Write-Warning "$($file.Name) is not validly signed ($($signature.Status))"
        continue
    }
    $thumbprint = [string]$signature.SignerCertificate.Thumbprint
    if ([string]::IsNullOrWhiteSpace($thumbprint)) {
        $invalid += $file.Name
        Write-Warning "$($file.Name) has no signer thumbprint"
        continue
    }
    $signerThumbprints += $thumbprint.ToUpperInvariant()
    Write-Output "$($file.Name) signed by $($signature.SignerCertificate.Subject) [$thumbprint]"
}

if ($RequireSigned -and $invalid.Count -gt 0) {
    throw "Unsigned release artifacts: $($invalid -join ', ')"
}
if ($RequireSameSigner) {
    if ($invalid.Count -gt 0) {
        throw "Cannot compare signing identities while artifacts are unsigned or invalid: $($invalid -join ', ')"
    }
    if (($signerThumbprints | Select-Object -Unique).Count -ne 1) {
        throw "Release artifacts are not signed by the same certificate"
    }
    Write-Output "All release artifacts use the same signing certificate"
}

if ($invalid.Count -eq 0) {
    Write-Output "Signing verification passed"
} else {
    Write-Output "Signing verification is advisory; provide a release certificate before public distribution"
}
