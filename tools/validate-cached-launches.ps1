param(
    [string]$BuildDir = (Join-Path $PSScriptRoot "..\cpp\build")
)

$ErrorActionPreference = "Stop"
$BuildDir = (Resolve-Path $BuildDir).Path
$launcher = Join-Path $BuildDir "amalgam_launcher.exe"
if (-not (Test-Path -LiteralPath $launcher)) { throw "Missing launcher: $launcher" }

$cases = @(
    [pscustomobject]@{ Version = "1.21.8"; Loader = "fabric" },
    [pscustomobject]@{ Version = "1.21.8"; Loader = "quilt" },
    [pscustomobject]@{ Version = "1.21.8"; Loader = "neoforge" },
    [pscustomobject]@{ Version = "1.12.2"; Loader = "forge" },
    [pscustomobject]@{ Version = "1.18.2"; Loader = "forge" },
    [pscustomobject]@{ Version = "1.19.2"; Loader = "forge" },
    [pscustomobject]@{ Version = "1.20.1"; Loader = "forge" }
)
# build-release.cmd wipes the build directory, so after a full gate the
# launch cache is cold: the first dry run downloads the whole asset set
# (~4300 objects) plus Java and jars, which is a network-speed test, not a
# launch-logic test. Warm it once with a generous budget; per-case runs then
# validate cached launch behavior within a tight timeout.
$warm = Join-Path $BuildDir "runtimes\java"
$first = $cases[0]
if (-not (Test-Path $warm)) {
    Write-Output "cold cache detected; warming with $($first.Version)/$($first.Loader) (no timeout)"
    $warmProc = Start-Process -FilePath $launcher `
        -ArgumentList @("--launch", $first.Version, $first.Loader, "--dry-run") `
        -WorkingDirectory $BuildDir -PassThru -WindowStyle Hidden
    if (-not $warmProc.WaitForExit(1800000)) {
        $warmProc.Kill()
        $warmProc.WaitForExit()
        throw "Cold-cache warm-up exceeded 30 minutes for $($first.Version)/$($first.Loader)"
    }
    if ($warmProc.ExitCode -ne 0) {
        throw "Cold-cache warm-up failed for $($first.Version)/$($first.Loader) (exit $($warmProc.ExitCode))"
    }
    Write-Output "cache warmed"
}

foreach ($case in $cases) {
    # The launcher is a GUI-subsystem executable: console callers cannot rely
    # on pipeline completion, so wait on the process with a timeout and kill
    # a hung run instead of blocking the QA session indefinitely.
    $proc = Start-Process -FilePath $launcher `
        -ArgumentList @("--launch", $case.Version, $case.Loader, "--dry-run") `
        -WorkingDirectory $BuildDir -PassThru -WindowStyle Hidden
    # 300s: a cached-profile dry run can still fetch one missing mod jar
    # (measured 200s for fabric-api over a slow link); 180s caused false fails.
    if (-not $proc.WaitForExit(300000)) {
        $proc.Kill()
        $proc.WaitForExit()
        throw "Cached dry-run timed out for $($case.Version)/$($case.Loader)"
    }
    if ($proc.ExitCode -ne 0) {
        throw "Cached dry-run failed for $($case.Version)/$($case.Loader) (exit $($proc.ExitCode))"
    }
    Write-Output "PASS $($case.Version)/$($case.Loader)"
}
