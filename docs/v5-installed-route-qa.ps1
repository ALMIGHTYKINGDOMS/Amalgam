$ErrorActionPreference = "Stop"
$root = Join-Path $env:TEMP "amalgam-v5-clean-install"
$exe = Join-Path $root "amalgam_launcher.exe"
$out = Join-Path $env:TEMP "amalgam-v5-route-snapshots"
$cwd = Join-Path $env:TEMP "amalgam-v5-route-cwd"
if (-not (Test-Path -LiteralPath $exe)) { throw "Installed launcher missing: $exe" }
if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Recurse -Force }
New-Item -ItemType Directory -Path $out -Force | Out-Null
New-Item -ItemType Directory -Path $cwd -Force | Out-Null
$routes = @("home", "discover", "library", "downloads", "essentials", "servers", "settings", "account", "java", "bedrock")
foreach ($size in @(@(1920, 1080), @(1600, 900), @(1366, 768))) {
    $width = $size[0]
    $height = $size[1]
    foreach ($route in $routes) {
        $png = Join-Path $out ("{0}-{1}x{2}.png" -f $route, $width, $height)
        $p = Start-Process -FilePath $exe -ArgumentList @("--ui-snapshot", $png, $width, $height, $route) -WorkingDirectory $cwd -Wait -PassThru
        if ($p.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $png)) {
            throw "Route snapshot failed: $route ${width}x${height}, exit $($p.ExitCode)"
        }
        $file = Get-Item -LiteralPath $png
        Write-Output ("SNAPSHOT {0} {1}x{2} {3} bytes" -f $route, $width, $height, $file.Length)
    }
}
$p = Start-Process -FilePath $exe -ArgumentList @("--check-prereqs") -WorkingDirectory $cwd -Wait -PassThru
Write-Output "PREREQ_EXIT=$($p.ExitCode)"
if ($p.ExitCode -ne 0) { throw "Installed prerequisite check failed" }
$p = Start-Process -FilePath $exe -ArgumentList @("--check-java") -WorkingDirectory $cwd -Wait -PassThru
Write-Output "JAVA_EXIT=$($p.ExitCode)"
if ($p.ExitCode -ne 0) { throw "Installed Java check failed" }
$p = Start-Process -FilePath $exe -ArgumentList @("--doctor") -WorkingDirectory $cwd -Wait -PassThru
Write-Output "DOCTOR_EXIT=$($p.ExitCode)"
Write-Output "ROUTE_QA_COMPLETE"
