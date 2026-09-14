param(
    [string]$PackageDir = 'dist/amalgam-1.0.0'
)

$ErrorActionPreference = 'Stop'
$packageDir = (Resolve-Path -LiteralPath $PackageDir).Path
$launcher = Join-Path $packageDir 'amalgam_launcher.exe'
if (-not (Test-Path -LiteralPath $launcher)) { throw "Launcher missing: $launcher" }

Push-Location -LiteralPath $packageDir
try {
    Write-Output '--- --check-prereqs ---'
    $prereqs = (& $launcher --check-prereqs 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0 -or $prereqs -notmatch 'tar=OK' -or $prereqs -notmatch 'amalgam.dll=OK') {
        throw "--check-prereqs failed: $prereqs"
    }
    Write-Output 'passed'

    Write-Output '--- --check-java ---'
    $java = (& $launcher --check-java 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0 -or $java -notmatch 'Java') { throw "--check-java failed: $java" }
    Write-Output 'passed'
}
finally {
    Pop-Location
}
