$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class AmalgamInputQa {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT point);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr hWnd, int command);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, int data, UIntPtr extra);
    [DllImport("user32.dll")] public static extern void keybd_event(byte key, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hWnd, uint message, UIntPtr wParam, IntPtr lParam);
    public const int SW_RESTORE = 9;
    public const uint DOWN = 0x0002;
    public const uint UP = 0x0004;
    public const uint KEY_UP = 0x0002;
    public const byte CTRL = 0x11;
    public const byte K = 0x4B;
    public const uint WM_CLOSE = 0x0010;
}
"@
$exe = (Resolve-Path "cpp/build-vs/amalgam_launcher.exe").Path
$root = (Resolve-Path "cpp/build-vs").Path
$out = Join-Path $root "screenshots\chaos"
New-Item -ItemType Directory -Path $out -Force | Out-Null
function Stop-Fresh {
    Get-Process -Name amalgam_launcher -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq $exe } |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 700
}
function Get-Fresh {
    for ($i = 0; $i -lt 80; $i++) {
        $p = Get-Process -Name amalgam_launcher -ErrorAction SilentlyContinue |
            Where-Object { $_.Path -eq $exe } | Select-Object -First 1
        if ($p -and $p.MainWindowHandle -ne 0) { return $p }
        Start-Sleep -Milliseconds 250
    }
    throw "launcher window did not start"
}
function Focus($p) {
    [AmalgamInputQa]::ShowWindowAsync($p.MainWindowHandle, [AmalgamInputQa]::SW_RESTORE) | Out-Null
    [AmalgamInputQa]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
}
function ClientBounds($p) {
    $r = [AmalgamInputQa+RECT]::new()
    [AmalgamInputQa]::GetClientRect($p.MainWindowHandle, [ref]$r) | Out-Null
    $pt = [AmalgamInputQa+POINT]::new()
    [AmalgamInputQa]::ClientToScreen($p.MainWindowHandle, [ref]$pt) | Out-Null
    return @{ Left = [int]$pt.X; Top = [int]$pt.Y; Width = [int]$r.Right; Height = [int]$r.Bottom }
}
function ClickClient([int]$x, [int]$y) {
    $p = Get-Fresh; Focus $p; $b = ClientBounds $p
    [AmalgamInputQa]::SetCursorPos($b.Left + $x, $b.Top + $y) | Out-Null
    [AmalgamInputQa]::mouse_event([AmalgamInputQa]::DOWN, 0, 0, 0, [UIntPtr]::Zero)
    [AmalgamInputQa]::mouse_event([AmalgamInputQa]::UP, 0, 0, 0, [UIntPtr]::Zero)
}
function CtrlK {
    [AmalgamInputQa]::keybd_event([AmalgamInputQa]::CTRL, 0, 0, [UIntPtr]::Zero)
    [AmalgamInputQa]::keybd_event([AmalgamInputQa]::K, 0, 0, [UIntPtr]::Zero)
    [AmalgamInputQa]::keybd_event([AmalgamInputQa]::K, 0, [AmalgamInputQa]::KEY_UP, [UIntPtr]::Zero)
    [AmalgamInputQa]::keybd_event([AmalgamInputQa]::CTRL, 0, [AmalgamInputQa]::KEY_UP, [UIntPtr]::Zero)
}
function Capture($p, [string]$name) {
    Focus $p; Start-Sleep -Milliseconds 300
    $r = [AmalgamInputQa+RECT]::new()
    [AmalgamInputQa]::GetClientRect($p.MainWindowHandle, [ref]$r) | Out-Null
    $width = [int]$r.Right; $height = [int]$r.Bottom
    $bmp = [System.Drawing.Bitmap]::new($width, $height)
    $g = [System.Drawing.Graphics]::FromImage($bmp); $hdc = $g.GetHdc()
    if (-not [AmalgamInputQa]::PrintWindow($p.MainWindowHandle, $hdc, 0)) { throw "capture failed" }
    $g.ReleaseHdc($hdc)
    $path = Join-Path $out ($name + ".png")
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Write-Output "CAPTURE $path"
}
Stop-Fresh
$p = Start-Process -FilePath $exe -ArgumentList "--safe-mode" -WorkingDirectory $root -PassThru
$p = Get-Fresh
Start-Sleep -Seconds 3
$rows = @(172, 204, 236, 268, 300, 332, 140)
for ($round = 0; $round -lt 12; $round++) {
    foreach ($row in $rows) {
        ClickClient 120 $row
        Start-Sleep -Milliseconds 35
    }
}
for ($round = 0; $round -lt 10; $round++) {
    Focus $p
    CtrlK
    Start-Sleep -Milliseconds 35
    [System.Windows.Forms.SendKeys]::SendWait("zzzz-no-result")
    [System.Windows.Forms.SendKeys]::SendWait("{ESC}")
    Start-Sleep -Milliseconds 35
}
Start-Sleep -Seconds 2
$p.Refresh()
if ($p.HasExited) { throw "launcher exited during rapid input stress" }
Capture $p "rapid_input_final"
[AmalgamInputQa]::PostMessageW($p.MainWindowHandle, [AmalgamInputQa]::WM_CLOSE, [UIntPtr]::Zero, [IntPtr]::Zero) | Out-Null
if (-not $p.WaitForExit(10000)) { $p | Stop-Process -Force; throw "launcher did not close after rapid input stress" }
Write-Output "RAPID_INPUT_PROCESS_ALIVE=True"
Write-Output "RAPID_INPUT_CLEAN_CLOSE=True"
Write-Output "RAPID_INPUT_PASS"
