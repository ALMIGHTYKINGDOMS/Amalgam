param(
    [ValidateSet("all", "install", "uninstall-yes", "uninstall-no")]
    [string]$Action = "all"
)

# Destructive-uninstall integration test for the production Setup.exe.
#
#   install       : seed disposable data, silent-install, verify files
#   uninstall-yes : run the interactive uninstaller and answer YES to the
#                   destructive prompt (Amalgam user data is removed too)
#   uninstall-no  : same, answering NO (data-preserving path)
#
# The .iss destructive branch only runs when UninstallSilent() is false, so
# the real interactive wizard is driven through UI Automation -- the same
# clicks a user would make. Real user data is backed up first and restored
# afterwards; the script aborts before deleting anything if backup
# verification fails.
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$setup = Join-Path $root "dist\installer\AmalgamLauncher-1.0.0-Setup.exe"
$local = $env:LOCALAPPDATA
$installDir = Join-Path $local "AmalgamLauncher"
$amalgamDir = Join-Path $local "Amalgam"
$stamp = Get-Date -Format yyyyMMddHHmmss
$seedInstance = Join-Path $local "instances\launch-test-profile"
$seedModpack = Join-Path $local "modpacks\launch-test-pack"
$seedBackup = Join-Path $local "backups\launch-test-backup"
$sessionFile = Join-Path $amalgamDir "account-sessions.json"

function Test-FileHas([string]$path, [string]$needle) {
    (Get-Content $path -Raw -ErrorAction SilentlyContinue) -match [regex]::Escape($needle)
}

function Backup-RealData {
    $backupRoot = Join-Path $env:TEMP "amalgam-qa-backup-$stamp"
    New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
    if (Test-Path $sessionFile) {
        $copy = Join-Path $backupRoot "account-sessions.json"
        Copy-Item $sessionFile $copy
        # Verify byte-identical: this file's shape can legitimately be an
        # empty session list, so content matching is not a validity check.
        if ((Get-FileHash $sessionFile).Hash -ne (Get-FileHash $copy).Hash) {
            throw "backup verification failed; aborting before any destructive step"
        }
    }
    return $backupRoot
}

function Restore-RealData([string]$backupRoot) {
    $saved = Join-Path $backupRoot "account-sessions.json"
    if (Test-Path $saved) {
        New-Item -ItemType Directory -Path $amalgamDir -Force | Out-Null
        Copy-Item $saved $sessionFile -Force
        if ((Get-FileHash $sessionFile).Hash -ne (Get-FileHash $saved).Hash) { throw "restore verification failed" }
        Write-Output "real user data restored (account-sessions.json)"
    }
}

function Invoke-DialogButton([string]$titlePattern, [string]$buttonName, [int]$timeoutSec = 30) {
    # Find the top-level dialog and invoke its Yes/No button by name.
    $auto = [System.Windows.Automation.AutomationElement]
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        $cond = New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ClassNameProperty, "#32770")
        $dialogs = $auto::RootElement.FindAll(
            [System.Windows.Automation.TreeScope]::Children, $cond)
        foreach ($d in $dialogs) {
            $name = $d.Current.Name
            if ($name -notmatch $titlePattern) { continue }
            $btnCond = New-Object System.Windows.Automation.AndCondition(
                (New-Object System.Windows.Automation.PropertyCondition(
                    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
                    [System.Windows.Automation.ControlType]::Button)),
                (New-Object System.Windows.Automation.PropertyCondition(
                    [System.Windows.Automation.AutomationElement]::NameProperty, $buttonName)))
            $btn = $d.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $btnCond)
            if ($btn) {
                $btnName = $btn.Current.Name
                (New-Object System.Windows.Automation.InvokePattern(
                    $btn.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern))).Invoke()
                Write-Output "clicked '$btnName' on dialog '$name'"
                return $true
            }
        }
        Start-Sleep -Milliseconds 700
    }
    return $false
}

function Wait-UninstallDone {
    # Inno's uninstaller re-execs itself, so waiting on the spawned process
    # can return before the file removal finishes. Wait until no uninstaller
    # process remains.
    $deadline = (Get-Date).AddSeconds(300)
    while ((Get-Date) -lt $deadline) {
        $running = Get-Process -Name "unins000", "_iu14D2N" -ErrorAction SilentlyContinue
        if (-not $running) { return $true }
        Start-Sleep -Seconds 2
    }
    return $false
}

if ($Action -in @("install", "all")) {    if (-not (Test-Path $setup)) { throw "Setup.exe missing: $setup" }
    New-Item -ItemType Directory -Path (Join-Path $seedInstance "mods") -Force | Out-Null
    @{ format = 1; id = "launch-test-profile"; name = "Launch Test Profile";
       minecraft_version = "1.20.1"; loader = "fabric" } |
        ConvertTo-Json | Set-Content (Join-Path $seedInstance "instance.json")
    New-Item -ItemType Directory -Path (Join-Path $seedModpack "overrides") -Force | Out-Null
    Set-Content (Join-Path $seedModpack "modpack.json") '{"name":"launch-test-pack"}'
    New-Item -ItemType Directory -Path $seedBackup -Force | Out-Null
    Set-Content (Join-Path $seedBackup "seed-marker.txt") "DESTRUCTIVE-TEST-$stamp"
    Write-Output "seeded instance/modpack/backup (marker $stamp)"

    $p = Start-Process -FilePath $setup -ArgumentList "/VERYSILENT", "/NORESTART", "/SUPPRESSMSGBOXES" -PassThru -Wait
    if ($p.ExitCode -ne 0) { throw "installer exit code $($p.ExitCode)" }
    foreach ($f in "amalgam_launcher.exe", "amalgam.dll", "unins000.exe") {
        if (-not (Test-Path (Join-Path $installDir $f))) { throw "installed file missing: $f" }
    }
    # Seed runtime-written configs in the install dir so the .iss DeleteFile
    # lines on the destructive path are genuinely exercised (the launcher
    # writes these on first run; simulate that before uninstalling).
    Set-Content (Join-Path $installDir "launcher.json") '{"runtime":"written-by-launcher"}'
    Set-Content (Join-Path $installDir "downloads.json") '{"active_downloads":{}}'
    Write-Output "installed (with uninstaller) to $installDir; seeded runtime configs"
    exit 0
}

if ($Action -in @("uninstall-yes", "uninstall-no", "all")) {
    $answer = if ($Action -eq "uninstall-no") { "No" } else { "Yes" }
    $backupRoot = Backup-RealData
    Write-Output "backed up real user data to $backupRoot"

    $unins = Join-Path $installDir "unins000.exe"
    if (-not (Test-Path $unins)) { throw "uninstaller missing: $unins" }
    Start-Process -FilePath $unins | Out-Null

    # Dialog 1: Inno's "This will remove Amalgam Launcher... Continue?" -> Yes.
    if (-not (Invoke-DialogButton "Amalgam" "Yes" 40)) { throw "uninstall confirmation dialog not found" }
    # Dialog 2: the .iss destructive prompt -> the answer under test.
    if (-not (Invoke-DialogButton "Amalgam" $answer 40)) { throw "destructive-prompt dialog not found" }
    if (-not (Wait-UninstallDone)) { throw "uninstaller did not finish" }

    foreach ($f in "amalgam_launcher.exe", "amalgam.dll", "unins000.exe", "launcher.json", "downloads.json") {
        if (Test-Path (Join-Path $installDir $f)) { throw "APP FILE SURVIVED UNINSTALL: $f" }
    }
    Write-Output "app files removed (install dir: $(Test-Path $installDir))"

    $expectDataGone = ($answer -eq "Yes")
    foreach ($dir in $amalgamDir, (Split-Path $seedInstance -Parent), (Split-Path $seedModpack -Parent), (Split-Path $seedBackup -Parent)) {
        $gone = -not (Test-Path $dir)
        if ($expectDataGone -and -not $gone) { throw "DESTRUCTIVE UNINSTALL LEFT DATA: $dir" }
        if (-not $expectDataGone -and $gone) { throw "NO-PATH DELETED DATA: $dir" }
    }
    if ($expectDataGone) {
        Write-Output "destructive path verified: Amalgam data dirs removed (Amalgam, instances, modpacks, backups)"
    } else {
        Write-Output "data-preserving path verified: Amalgam data dirs intact"
    }

    Restore-RealData $backupRoot

    if (-not $expectDataGone) {
        # Leave the machine as found: remove the seeds the destructive path
        # would have removed, and re-verify the app dir is gone.
        Remove-Item -Recurse -Force $seedInstance, $seedModpack, $seedBackup -ErrorAction SilentlyContinue
        Write-Output "seed data cleaned up"
    }
    if (Test-Path $installDir) { Remove-Item -Recurse -Force $installDir }
    Write-Output "machine state restored"
    exit 0
}

throw "unknown action: $Action"