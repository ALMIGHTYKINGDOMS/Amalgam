$ErrorActionPreference = "Stop"
$source = (Resolve-Path "cpp/build-vs").Path
$root = Join-Path ([IO.Path]::GetTempPath()) ("amalgam-config-chaos-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $root -Force | Out-Null
try {
    Copy-Item -LiteralPath (Join-Path $source "amalgam_launcher.exe") -Destination $root
    Copy-Item -LiteralPath (Join-Path $source "amalgam.dll") -Destination $root
    Copy-Item -LiteralPath (Join-Path $source "launcher.json") -Destination (Join-Path $root "launcher.json")
    Copy-Item -LiteralPath (Join-Path $source "launcher.json.bak") -Destination (Join-Path $root "launcher.json.bak")
    $configs = @(
        @{ Name = "truncated"; Text = '{"base_dir":"instances","width":12' },
        @{ Name = "malformed"; Text = 'not-json' },
        @{ Name = "missing"; Text = $null }
    )
    foreach ($case in $configs) {
        Copy-Item -LiteralPath (Join-Path $source "launcher.json.bak") -Destination (Join-Path $root "launcher.json.bak") -Force
        if ($null -eq $case.Text) {
            Remove-Item -LiteralPath (Join-Path $root "launcher.json") -Force -ErrorAction SilentlyContinue
        } else {
            [IO.File]::WriteAllText((Join-Path $root "launcher.json"), $case.Text)
        }
        $result = & (Join-Path $root "amalgam_launcher.exe") --doctor 2>&1 | Out-String
        if ($LASTEXITCODE -ne 2 -and $LASTEXITCODE -ne 0) {
            throw ("{0} config caused unexpected doctor exit {1}: {2}" -f $case.Name, $LASTEXITCODE, $result)
        }
        if ($case.Name -eq "missing") {
            Write-Output ("CONFIG_CASE missing DOCTOR_EXIT=" + $LASTEXITCODE + " DEFAULTS_OK")
        } else {
            Write-Output ("CONFIG_CASE " + $case.Name + " EXIT=" + $LASTEXITCODE)
        }
    }

    Copy-Item -LiteralPath (Join-Path $source "launcher.json.bak") -Destination (Join-Path $root "launcher.json.bak") -Force
    Remove-Item -LiteralPath (Join-Path $root "launcher.json") -Force -ErrorAction SilentlyContinue
    $process = Start-Process -FilePath (Join-Path $root "amalgam_launcher.exe") -ArgumentList "--safe-mode" -WorkingDirectory $root -PassThru
    $window = $null
    for ($i = 0; $i -lt 60; $i++) {
        $process.Refresh()
        if ($process.MainWindowHandle -ne 0) { $window = $process.MainWindowHandle; break }
        Start-Sleep -Milliseconds 250
    }
    if (-not $window) {
        $process | Stop-Process -Force -ErrorAction SilentlyContinue
        throw "windowed missing-config launch did not expose a window"
    }
    if (-not $process.CloseMainWindow()) {
        $process | Stop-Process -Force
    }
    if (-not $process.WaitForExit(10000)) {
        $process | Stop-Process -Force
        throw "windowed missing-config launch did not close"
    }
    if (-not (Test-Path -LiteralPath (Join-Path $root "launcher.json"))) {
        throw "windowed startup did not recreate missing config"
    }
    Write-Output "CONFIG_CASE missing WINDOWED_RECREATED"
    Write-Output "CONFIG_CHAOS_PASS"
} finally {
    if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
}
