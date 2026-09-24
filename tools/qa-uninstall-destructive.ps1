param(
    [ValidateSet("all", "install", "uninstall-yes", "uninstall-no", "audit")]
    [string]$Action = "all",
    [string]$SourceDir = (Join-Path $PSScriptRoot "..\dist\amalgam-1.0.0"),
    [string]$QaRoot = (Join-Path ([IO.Path]::GetTempPath()) ("amalgam-uninstall-qa-" + [Guid]::NewGuid().ToString("N")))
)

# Isolated destructive-uninstall integration test.
#
# This test NEVER installs, seeds, backs up, restores, or removes a user's
# real Amalgam installation or LocalAppData. It compiles a uniquely identified
# QA installer with compile-time paths below a verified temporary sandbox, then
# drives that isolated installer and uninstaller through the same UI a customer
# sees. Production user-data roots are intentionally out of scope for this
# test because a partial backup cannot make their deletion safe.
#
#   audit          : validate sandbox/installer prerequisites only
#   install        : compile and silent-install one isolated QA instance
#   uninstall-yes : install an isolated instance, choose the destructive path,
#                   and verify only its sandbox data is removed
#   uninstall-no  : install an isolated instance, choose the preserving path,
#                   and verify only its sandbox data remains
#   all            : run both isolated uninstall scenarios

$ErrorActionPreference = "Stop"
$script:QaSentinelName = ".amalgam-uninstall-qa-sandbox"
$script:QaSentinelText = "AMALGAM-UNINSTALL-QA-SANDBOX-v1"
$script:QaTitle = "Amalgam Launcher QA"

$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$iss = Join-Path $root "installer\AmalgamLauncher.iss"
$SourceDir = (Resolve-Path -LiteralPath $SourceDir).Path

function Get-NormalizedPath([string]$Path) {
    return [IO.Path]::GetFullPath($Path).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
}

function Assert-QARoot([string]$Path, [switch]$RequireSentinel) {
    $full = Get-NormalizedPath $Path
    $tempRoot = Get-NormalizedPath ([IO.Path]::GetTempPath())
    $tempPrefix = $tempRoot + [IO.Path]::DirectorySeparatorChar
    if (-not $full.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "QA root must be below the system temporary directory: $full"
    }
    $leaf = Split-Path -Leaf $full
    if (-not $leaf.StartsWith("amalgam-uninstall-qa-", [StringComparison]::OrdinalIgnoreCase)) {
        throw "QA root must use the guarded amalgam-uninstall-qa- prefix: $full"
    }
    if ($RequireSentinel) {
        $sentinel = Join-Path $full $script:QaSentinelName
        if (-not (Test-Path -LiteralPath $sentinel) -or
            ((Get-Content -LiteralPath $sentinel -Raw).Trim() -ne $script:QaSentinelText)) {
            throw "Refusing to alter a QA root without the expected sandbox sentinel: $full"
        }
    }
    return $full
}

function Initialize-QARoot([string]$Path) {
    $full = Assert-QARoot $Path
    if (Test-Path -LiteralPath $full) {
        throw "QA root already exists; choose a new empty sandbox path: $full"
    }
    New-Item -ItemType Directory -Path $full -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $full $script:QaSentinelName), $script:QaSentinelText,
        [Text.UTF8Encoding]::new($false))
    return (Assert-QARoot $full -RequireSentinel)
}

function Remove-QARoot([string]$Path) {
    $full = Assert-QARoot $Path -RequireSentinel
    Remove-Item -LiteralPath $full -Recurse -Force
}

function Get-ISCC {
    foreach ($candidate in @(
        (Get-Command ISCC.exe -ErrorAction SilentlyContinue).Source,
        (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 7\ISCC.exe"),
        (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe"),
        "C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
        "C:\Program Files (x86)\Inno Setup 7\ISCC.exe",
        "C:\Program Files\Inno Setup 6\ISCC.exe",
        "C:\Program Files\Inno Setup 7\ISCC.exe")) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) { return $candidate }
    }
    throw "ISCC.exe is required to compile the isolated QA installer."
}

function Assert-InstallerInputs {
    if (-not (Test-Path -LiteralPath $iss)) { throw "Installer source missing: $iss" }
    foreach ($relative in @("amalgam_launcher.exe", "amalgam.dll", "bridges", "LICENSE.txt", "INSTALL-INFO.txt")) {
        if (-not (Test-Path -LiteralPath (Join-Path $SourceDir $relative))) {
            throw "QA installer input missing: $relative"
        }
    }
}

function New-QAInstaller([string]$Sandbox) {
    $Sandbox = Assert-QARoot $Sandbox -RequireSentinel
    Assert-InstallerInputs
    $iscc = Get-ISCC
    $outputDir = Join-Path $Sandbox "installer"
    $installDir = Join-Path $Sandbox "app"
    $dataRoot = Join-Path $Sandbox "data"
    New-Item -ItemType Directory -Path $outputDir -Force | Out-Null

    # The separate AppId ensures Inno Setup cannot treat a production install
    # as an upgrade, replacement, or uninstall target. Double opening braces
    # are intentional Inno syntax for a literal GUID opening brace.
    $qaAppId = "{{" + [Guid]::NewGuid().ToString() + "}"
    & $iscc "/DSourceDir=$SourceDir" "/DMyAppVersion=1.0.0-qa" "/DMyFileVersion=1.0.0.0" `
        "/DMyAppName=$script:QaTitle" "/DInstallerAppId=$qaAppId" `
        "/DDefaultInstallDir=$installDir" "/DUserDataRoot=$dataRoot" `
        "/DOutputDir=$outputDir" "/DOutputBaseFilename=AmalgamLauncher-QA-Setup" $iss
    if ($LASTEXITCODE -ne 0) { throw "Isolated installer compilation failed with exit code $LASTEXITCODE" }

    $setup = Join-Path $outputDir "AmalgamLauncher-QA-Setup.exe"
    if (-not (Test-Path -LiteralPath $setup)) { throw "Isolated setup executable missing: $setup" }
    return [pscustomobject]@{
        Sandbox = $Sandbox
        Setup = $setup
        InstallDir = $installDir
        DataRoot = $dataRoot
        AmalgamDir = (Join-Path $dataRoot "Amalgam")
        InstancesDir = (Join-Path $dataRoot "instances")
        ModpacksDir = (Join-Path $dataRoot "modpacks")
        BackupsDir = (Join-Path $dataRoot "backups")
    }
}

function Write-QAFile([string]$Path, [string]$Text) {
    New-Item -ItemType Directory -Path (Split-Path -Parent $Path) -Force | Out-Null
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}

function Seed-QAData($qa) {
    Write-QAFile -Path (Join-Path $qa.InstancesDir "launch-test-profile\instance.json") -Text '{"format":1,"id":"launch-test-profile","name":"Launch Test Profile","minecraft_version":"1.20.1","loader":"fabric"}'
    Write-QAFile -Path (Join-Path $qa.ModpacksDir "launch-test-pack\modpack.json") -Text '{"name":"launch-test-pack"}'
    Write-QAFile -Path (Join-Path $qa.BackupsDir "launch-test-backup\seed-marker.txt") -Text "DESTRUCTIVE-TEST"
    Write-QAFile -Path (Join-Path $qa.AmalgamDir "account-sessions.json") -Text '[]'
}

function Install-QAInstance($qa) {
    $p = Start-Process -FilePath $qa.Setup -ArgumentList @("/VERYSILENT", "/NORESTART", "/SUPPRESSMSGBOXES", '/COMPONENTS="launcher"') -PassThru -Wait
    if ($p.ExitCode -ne 0) { throw "isolated installer exit code $($p.ExitCode)" }
    foreach ($file in @("amalgam_launcher.exe", "amalgam.dll", "unins000.exe")) {
        if (-not (Test-Path -LiteralPath (Join-Path $qa.InstallDir $file))) {
            throw "isolated installed file missing: $file"
        }
    }
    Write-QAFile (Join-Path $qa.InstallDir "launcher.json") '{"runtime":"qa"}'
    Write-QAFile (Join-Path $qa.InstallDir "downloads.json") '{"active_downloads":{}}'
}

function Invoke-DialogButton([string]$buttonName, [int]$timeoutSec = 40) {
    $auto = [System.Windows.Automation.AutomationElement]
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        $condition = New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ClassNameProperty, "#32770")
        $dialogs = $auto::RootElement.FindAll([System.Windows.Automation.TreeScope]::Children, $condition)
        foreach ($dialog in $dialogs) {
            if ($dialog.Current.Name -notmatch [regex]::Escape($script:QaTitle)) { continue }
            $buttonCondition = New-Object System.Windows.Automation.AndCondition(
                (New-Object System.Windows.Automation.PropertyCondition(
                    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
                    [System.Windows.Automation.ControlType]::Button)),
                (New-Object System.Windows.Automation.PropertyCondition(
                    [System.Windows.Automation.AutomationElement]::NameProperty, $buttonName)))
            $button = $dialog.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $buttonCondition)
            if ($button) {
                (New-Object System.Windows.Automation.InvokePattern(
                    $button.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern))).Invoke()
                Write-Output "clicked '$buttonName' on isolated QA dialog"
                return $true
            }
        }
        Start-Sleep -Milliseconds 700
    }
    return $false
}

function Wait-QAUninstall([string]$InstallDir) {
    $deadline = (Get-Date).AddSeconds(300)
    while ((Get-Date) -lt $deadline) {
        if (-not (Test-Path -LiteralPath (Join-Path $InstallDir "unins000.exe")) -and
            -not (Test-Path -LiteralPath (Join-Path $InstallDir "amalgam_launcher.exe"))) {
            return $true
        }
        Start-Sleep -Seconds 2
    }
    return $false
}

function Assert-QAUninstallResult($qa, [string]$Answer) {
    foreach ($file in @("amalgam_launcher.exe", "amalgam.dll", "unins000.exe", "launcher.json", "downloads.json")) {
        if (Test-Path -LiteralPath (Join-Path $qa.InstallDir $file)) {
            throw "isolated app file survived uninstall: $file"
        }
    }
    $expectDataGone = $Answer -eq "Yes"
    foreach ($dir in @($qa.AmalgamDir, $qa.InstancesDir, $qa.ModpacksDir, $qa.BackupsDir)) {
        $gone = -not (Test-Path -LiteralPath $dir)
        if ($expectDataGone -and -not $gone) { throw "isolated destructive uninstall left data: $dir" }
        if (-not $expectDataGone -and $gone) { throw "isolated preserving uninstall deleted data: $dir" }
    }
}

function Invoke-QAScenario([string]$Name, [string]$Answer = "") {
    $sandbox = Initialize-QARoot $Name
    try {
        $qa = New-QAInstaller $sandbox
        Seed-QAData $qa
        Install-QAInstance $qa
        if ([string]::IsNullOrEmpty($Answer)) {
            Write-Output "isolated install verified: $($qa.InstallDir)"
            return
        }

        Add-Type -AssemblyName UIAutomationClient
        Add-Type -AssemblyName UIAutomationTypes
        $uninstaller = Join-Path $qa.InstallDir "unins000.exe"
        Start-Process -FilePath $uninstaller | Out-Null
        if (-not (Invoke-DialogButton "Yes")) { throw "isolated uninstall confirmation dialog not found" }
        if (-not (Invoke-DialogButton $Answer)) { throw "isolated destructive-prompt dialog not found" }
        if (-not (Wait-QAUninstall $qa.InstallDir)) { throw "isolated uninstaller did not finish" }
        Assert-QAUninstallResult $qa $Answer
        Write-Output "isolated uninstall '$Answer' scenario verified"
    }
    finally {
        if (Test-Path -LiteralPath $sandbox) { Remove-QARoot $sandbox }
    }
}

Assert-InstallerInputs
if ($Action -eq "audit") {
    $checkedRoot = Assert-QARoot $QaRoot
    Write-Output "QA uninstall audit passed: installer inputs and guarded temporary root are valid ($checkedRoot)"
    exit 0
}

if ($Action -eq "install") {
    Invoke-QAScenario $QaRoot
    exit 0
}

if ($Action -eq "uninstall-yes") {
    Invoke-QAScenario $QaRoot "Yes"
    exit 0
}

if ($Action -eq "uninstall-no") {
    Invoke-QAScenario $QaRoot "No"
    exit 0
}

Invoke-QAScenario ($QaRoot + "-yes") "Yes"
Invoke-QAScenario ($QaRoot + "-no") "No"
Write-Output "all isolated uninstall scenarios passed"
