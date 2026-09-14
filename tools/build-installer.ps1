param(
    [Parameter(Mandatory = $true)]
    [string]$SourceDir,
    [string]$Version = "0.2.0",
    [string]$OutputDir = ""
)

$ErrorActionPreference = "Stop"
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $ScriptRoot "..\dist\installer"
}
$SourceDir = (Resolve-Path $SourceDir).Path
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
$script = (Resolve-Path (Join-Path $ScriptRoot "..\installer\AmalgamLauncher.iss")).Path
$iscc = $null
foreach ($candidate in @(
    (Get-Command ISCC.exe -ErrorAction SilentlyContinue).Source,
    (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 7\ISCC.exe"),
    (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe"),
    "C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
    "C:\Program Files (x86)\Inno Setup 7\ISCC.exe",
    "C:\Program Files\Inno Setup 6\ISCC.exe",
    "C:\Program Files\Inno Setup 7\ISCC.exe")) {
    if ($candidate -and (Test-Path -LiteralPath $candidate)) { $iscc = $candidate; break }
}
if (-not $iscc) { throw "ISCC.exe is not installed. Install Inno Setup before compiling the installer." }
if (-not (Test-Path -LiteralPath (Join-Path $SourceDir "USER-GUIDE.md"))) {
    throw "Validated staging directory is missing USER-GUIDE.md"
}
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$numericParts = @([regex]::Matches($Version, '\d+') | ForEach-Object { [int]$_.Value })
while ($numericParts.Count -lt 4) { $numericParts += 0 }
$fileVersion = ($numericParts[0..3] -join '.')
$safeVersion = ($Version -replace '[^0-9A-Za-z._-]', '-')
$outputName = "AmalgamLauncher-$safeVersion-Setup"
& $iscc "/DSourceDir=$SourceDir" "/DMyAppVersion=$Version" "/DMyFileVersion=$fileVersion" `
    "/DOutputDir=$OutputDir" "/DOutputBaseFilename=$outputName" $script
if ($LASTEXITCODE -ne 0) { throw "Inno Setup compilation failed with exit code $LASTEXITCODE" }
Write-Output "Installer created in $OutputDir"
