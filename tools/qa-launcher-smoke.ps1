param(
    [string]$PackageDir = 'dist/amalgam-1.0.0'
)

# An unknown flag used to warn and then open the GUI, and a flag in --launch's
# optional loader slot was read as a loader name. Both are cheap and hermetic
# to assert here.

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

    Write-Output '--- --help / --version ---'
    $help = (& $launcher --help 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0 -or $help -notmatch '--launch' -or $help -notmatch '--help') {
        throw "--help failed: $help"
    }
    $version = (& $launcher --version 2>&1 | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $version -notmatch '^\d+\.\d+\.\d+$') { throw "--version failed: $version" }
    Write-Output "passed ($version)"

    Write-Output '--- --launch argument parsing ---'
    # Fails fast without touching the network, and must name the version rather
    # than blaming a loader.
    $badLaunch = (& $launcher --launch amalgam-no-such-version --wait --dry-run 2>&1 | Out-String)
    if ($badLaunch -notmatch 'unknown Minecraft version' -or $badLaunch -match 'unknown loader') {
        throw "--launch argument parsing regressed: $badLaunch"
    }
    Write-Output 'passed'
}
finally {
    Pop-Location
}

# The --launch check deliberately exercises a failing invocation, which leaves
# $LASTEXITCODE non-zero for callers (the release gate) even though every check
# above passed. Exit explicitly.
exit 0
