$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class AmalgamRecoveryQa {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT point);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr hWnd, int command);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hWnd, uint message, UIntPtr wParam, IntPtr lParam);
    public const int SW_RESTORE = 9;
    public const uint WM_CLOSE = 0x0010;
}
"@
$exe = (Resolve-Path "cpp/build-vs/amalgam_launcher.exe").Path
$root = (Resolve-Path "cpp/build-vs").Path
$marker = Join-Path $root ".amalgam_running"
$out = Join-Path $root "screenshots\chaos"
New-Item -ItemType Directory -Path $out -Force | Out-Null
function Stop-Fresh {
    Get-Process -Name amalgam_launcher -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq $exe } |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 700
}
function Get-Fresh {
    for ($i = 0; $i -lt 60; $i++) {
        $p = Get-Process -Name amalgam_launcher -ErrorAction SilentlyContinue |
            Where-Object { $_.Path -eq $exe } | Select-Object -First 1
        if ($p -and $p.MainWindowHandle -ne 0) { return $p }
        Start-Sleep -Milliseconds 250
    }
    throw "launcher window did not start"
}
function Capture($p, [string]$name) {
    [AmalgamRecoveryQa]::ShowWindowAsync($p.MainWindowHandle, [AmalgamRecoveryQa]::SW_RESTORE) | Out-Null
    [AmalgamRecoveryQa]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
    Start-Sleep -Milliseconds 300
    $r = [AmalgamRecoveryQa+RECT]::new()
    [AmalgamRecoveryQa]::GetClientRect($p.MainWindowHandle, [ref]$r) | Out-Null
    $width = [int]$r.Right - [int]$r.Left
    $height = [int]$r.Bottom - [int]$r.Top
    $bmp = [System.Drawing.Bitmap]::new($width, $height)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    if (-not [AmalgamRecoveryQa]::PrintWindow($p.MainWindowHandle, $hdc, 0)) { throw "PrintWindow failed" }
    $g.ReleaseHdc($hdc)
    $path = Join-Path $out ($name + ".png")
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Write-Output "CAPTURE $path"
}
Stop-Fresh
if (Test-Path -LiteralPath $marker) { Remove-Item -LiteralPath $marker -Force }
$first = Start-Process -FilePath $exe -WorkingDirectory $root -PassThru
$p = Get-Fresh
Start-Sleep -Seconds 2
Stop-Process -Id $p.Id -Force
Start-Sleep -Milliseconds 700
$markerAfterKill = Test-Path -LiteralPath $marker
if (-not $markerAfterKill) { throw "forced termination did not leave recovery marker" }
$second = Start-Process -FilePath $exe -WorkingDirectory $root -PassThru
$p2 = Get-Fresh
Start-Sleep -Seconds 2
Capture $p2 "recovery_dialog"
[AmalgamRecoveryQa]::PostMessageW($p2.MainWindowHandle, [AmalgamRecoveryQa]::WM_CLOSE, [UIntPtr]::Zero, [IntPtr]::Zero) | Out-Null
$p2.WaitForExit(10000)
Start-Sleep -Milliseconds 500
$markerAfterCleanClose = Test-Path -LiteralPath $marker
Write-Output "FORCED_TERMINATION_MARKER=$markerAfterKill"
Write-Output "CLEAN_CLOSE_MARKER=$markerAfterCleanClose"
if ($markerAfterCleanClose) { throw "clean shutdown did not remove recovery marker" }
Write-Output "RECOVERY_CHAOS_PASS"
