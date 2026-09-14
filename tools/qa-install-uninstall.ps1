param([string]$Action = "install")

# Real install/uninstall integration test for the production Setup.exe.
# install  : seeds disposable data, silent-installs, verifies installed files
#            and that the installed launcher runs from its own directory.
# uninstall: silent-uninstalls (the data-preserving path — the destructive
#            prompt never appears unattended), verifies app files are gone and
#            user data survived, then removes the disposable seed data.
$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$setup = Join-Path $root "dist\installer\AmalgamLauncher-1.0.0-Setup.exe"
$seedMarker = "LAUNCH-TEST-$(Get-Date -Format yyyyMMddHHmmss)"
$local = $env:LOCALAPPDATA
$installDir = Join-Path $local "AmalgamLauncher"
$seedInstance = Join-Path $local "instances\launch-test-profile"
$seedBackup = Join-Path $local "backups\launch-test-backup"

if ($Action -eq "install") {
    if (-not (Test-Path $setup)) { throw "Setup.exe missing: $setup" }

    # Seed disposable Amalgam-owned data the uninstaller must PRESERVE.
    New-Item -ItemType Directory -Path (Join-Path $seedInstance "mods") -Force | Out-Null
    @{ format = 1; id = "launch-test-profile"; name = "Launch Test Profile";
       minecraft_version = "1.20.1"; loader = "fabric" } |
        ConvertTo-Json | Set-Content (Join-Path $seedInstance "instance.json")
    Set-Content (Join-Path (Join-Path $seedInstance "mods") "seed-marker.txt") $seedMarker
    New-Item -ItemType Directory -Path $seedBackup -Force | Out-Null
    Set-Content (Join-Path $seedBackup "seed-marker.txt") $seedMarker
    Write-Output "seeded disposable instance + backup (marker $seedMarker)"

    Write-Output "running silent install..."
    $p = Start-Process -FilePath $setup -ArgumentList "/VERYSILENT", "/NORESTART", "/SUPPRESSMSGBOXES" -PassThru -Wait
    if ($p.ExitCode -ne 0) { throw "installer exit code $($p.ExitCode)" }

    foreach ($f in "amalgam_launcher.exe", "amalgam.dll", "launcher.json.template", "bridges", "bedrock") {
        if (-not (Test-Path (Join-Path $installDir $f))) { throw "installed file missing: $f" }
    }
    Write-Output "installed files verified in $installDir"

    $installedExe = Join-Path $installDir "amalgam_launcher.exe"
    $out = & $installedExe --check-prereqs 2>&1 | Out-String
    if ($out -notmatch "tar=OK" -or $out -notmatch "amalgam.dll=OK") {
        throw "installed launcher prereq check failed: $out"
    }
    Write-Output "installed launcher --check-prereqs OK"

    $reg = Get-ItemProperty "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*",
                            "HKCU:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*" `
               -ErrorAction SilentlyContinue |
           Where-Object { $_.DisplayName -like "*Amalgam*" } | Select-Object -First 1
    if (-not $reg) { throw "no Amalgam uninstall registry entry found" }
    Write-Output "registry entry: $($reg.DisplayName) $($reg.DisplayVersion) -> $($reg.UninstallString)"
    exit 0
}

if ($Action -eq "uninstall") {
    $reg = Get-ItemProperty "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*",
                            "HKCU:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*" `
               -ErrorAction SilentlyContinue |
           Where-Object { $_.DisplayName -like "*Amalgam*" } | Select-Object -First 1
    if (-not $reg) { throw "Amalgam is not installed (nothing to uninstall)" }

    Write-Output "running silent uninstall (data-preserving path)..."
    $uninst = $reg.UninstallString -replace '"', ''
    $p = Start-Process -FilePath $uninst -ArgumentList "/VERYSILENT", "/NORESTART", "/SUPPRESSMSGBOXES" -PassThru -Wait
    if ($p.ExitCode -ne 0) { throw "uninstaller exit code $($p.ExitCode)" }
    Start-Sleep -Seconds 3

    if (Test-Path (Join-Path $installDir "amalgam_launcher.exe")) {
        throw "uninstall left amalgam_launcher.exe behind"
    }
    Write-Output "application files removed"

    if (-not (Test-Path (Join-Path $seedInstance "instance.json"))) {
        throw "USER DATA LOST: seeded instance was deleted by uninstall"
    }
    if ((Get-Content (Join-Path $seedBackup "seed-marker.txt") -ErrorAction SilentlyContinue) -notlike "LAUNCH-TEST-*") {
        throw "USER DATA LOST: seeded backup content changed"
    }
    Write-Output "user data preserved (instance + backup intact)"

    # Clean up the disposable seed so the machine is left as found.
    Remove-Item -Recurse -Force $seedInstance, $seedBackup
    Write-Output "seed data cleaned up; machine state restored"
    exit 0
}

throw "unknown action: $Action (use install or uninstall)"
