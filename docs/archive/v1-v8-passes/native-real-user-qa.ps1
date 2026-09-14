$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class AmalgamNativeQa {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT point);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, uint nFlags);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, int data, UIntPtr extra);
    [DllImport("user32.dll")] public static extern void keybd_event(byte key, byte scan, uint flags, UIntPtr extra);
    public const int SW_RESTORE = 9;
    public const uint MOUSE_LEFT_DOWN = 0x0002;
    public const uint MOUSE_LEFT_UP = 0x0004;
    public const uint MOUSE_WHEEL = 0x0800;
    public const uint KEY_UP = 0x0002;
    public const byte VK_CONTROL = 0x11;
    public const byte VK_K = 0x4B;
    public const byte VK_ESCAPE = 0x1B;
    public const byte VK_ENTER = 0x0D;
}
"@

$exe = (Resolve-Path "cpp/build-vs/amalgam_launcher.exe").Path
$out = (Resolve-Path "cpp/build-vs").Path + "\screenshots\native-user-qa"
New-Item -ItemType Directory -Path $out -Force | Out-Null

function Get-LauncherProcess {
    $p = Get-Process -Name amalgam_launcher -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq $exe } | Select-Object -First 1
    if (-not $p) { throw "Fresh launcher process is not running: $exe" }
    if ($p.MainWindowHandle -eq 0) { throw "Launcher has no native window handle" }
    return $p
}
function Get-ClientBounds($p) {
    $r = New-Object AmalgamNativeQa+RECT
    [AmalgamNativeQa]::GetClientRect($p.MainWindowHandle, [ref]$r) | Out-Null
    $pt = New-Object AmalgamNativeQa+POINT
    [AmalgamNativeQa]::ClientToScreen($p.MainWindowHandle, [ref]$pt) | Out-Null
    return @{ Left=$pt.X; Top=$pt.Y; Width=$r.Right-$r.Left; Height=$r.Bottom-$r.Top }
}
function Focus-Launcher($p) {
    [AmalgamNativeQa]::ShowWindowAsync($p.MainWindowHandle, [AmalgamNativeQa]::SW_RESTORE) | Out-Null
    [AmalgamNativeQa]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
    Start-Sleep -Milliseconds 300
}
function Capture-Launcher([string]$name) {
    $p = Get-LauncherProcess
    Focus-Launcher $p
    $b = Get-ClientBounds $p
    $bmp = New-Object System.Drawing.Bitmap($b.Width, $b.Height)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    $printed = [AmalgamNativeQa]::PrintWindow($p.MainWindowHandle, $hdc, 0)
    $g.ReleaseHdc($hdc)
    if (-not $printed) { throw "PrintWindow failed for launcher" }
    $path = Join-Path $out ($name + ".png")
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Write-Output "CAPTURE $name $($b.Width)x$($b.Height)"
}
function Click-Launcher([int]$x, [int]$y) {
    $p = Get-LauncherProcess; Focus-Launcher $p; $b = Get-ClientBounds $p
    [AmalgamNativeQa]::SetCursorPos($b.Left + $x, $b.Top + $y) | Out-Null
    Start-Sleep -Milliseconds 100
    [AmalgamNativeQa]::mouse_event([AmalgamNativeQa]::MOUSE_LEFT_DOWN, 0, 0, 0, [UIntPtr]::Zero)
    [AmalgamNativeQa]::mouse_event([AmalgamNativeQa]::MOUSE_LEFT_UP, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 900
    Write-Output "CLICK $x,$y"
}
function Scroll-Launcher([int]$notches) {
    $p = Get-LauncherProcess; Focus-Launcher $p; $b = Get-ClientBounds $p
    [AmalgamNativeQa]::SetCursorPos($b.Left + [int]($b.Width * 0.72), $b.Top + [int]($b.Height * 0.78)) | Out-Null
    [AmalgamNativeQa]::mouse_event([AmalgamNativeQa]::MOUSE_WHEEL, 0, 0, $notches * 120, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 800
    Write-Output "SCROLL $notches"
}
function Open-GlobalSearch {
    $p = Get-LauncherProcess; Focus-Launcher $p
    [AmalgamNativeQa]::keybd_event([AmalgamNativeQa]::VK_CONTROL, 0, 0, [UIntPtr]::Zero)
    [AmalgamNativeQa]::keybd_event([AmalgamNativeQa]::VK_K, 0, 0, [UIntPtr]::Zero)
    [AmalgamNativeQa]::keybd_event([AmalgamNativeQa]::VK_K, 0, [AmalgamNativeQa]::KEY_UP, [UIntPtr]::Zero)
    [AmalgamNativeQa]::keybd_event([AmalgamNativeQa]::VK_CONTROL, 0, [AmalgamNativeQa]::KEY_UP, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 500
    Write-Output "SEARCH_OPEN"
}
function Navigate-Launcher([byte]$key) {
    $p = Get-LauncherProcess; Focus-Launcher $p
    [AmalgamNativeQa]::keybd_event([AmalgamNativeQa]::VK_CONTROL, 0, 0, [UIntPtr]::Zero)
    [AmalgamNativeQa]::keybd_event($key, 0, 0, [UIntPtr]::Zero)
    [AmalgamNativeQa]::keybd_event($key, 0, [AmalgamNativeQa]::KEY_UP, [UIntPtr]::Zero)
    [AmalgamNativeQa]::keybd_event([AmalgamNativeQa]::VK_CONTROL, 0, [AmalgamNativeQa]::KEY_UP, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 900
    Write-Output "NAVIGATE CTRL+$([char]$key)"
}
function Navigate-Settings {
    $p = Get-LauncherProcess; Focus-Launcher $p
    [AmalgamNativeQa]::keybd_event([AmalgamNativeQa]::VK_CONTROL, 0, 0, [UIntPtr]::Zero)
    [AmalgamNativeQa]::keybd_event(188, 0, 0, [UIntPtr]::Zero)
    [AmalgamNativeQa]::keybd_event(188, 0, [AmalgamNativeQa]::KEY_UP, [UIntPtr]::Zero)
    [AmalgamNativeQa]::keybd_event([AmalgamNativeQa]::VK_CONTROL, 0, [AmalgamNativeQa]::KEY_UP, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 900
    Write-Output "NAVIGATE CTRL+,"
}
function Type-Text([string]$text) {
    [System.Windows.Forms.SendKeys]::SendWait($text)
    Start-Sleep -Milliseconds 1200
    Write-Output "TYPE $text"
}
function Press-Escape {
    [System.Windows.Forms.SendKeys]::SendWait("{ESC}")
    Start-Sleep -Milliseconds 500
}

$p = Get-LauncherProcess
Capture-Launcher "home_top"
Click-Launcher 120 172
Capture-Launcher "discover_top"
Scroll-Launcher -5
Capture-Launcher "discover_middle_01"
Scroll-Launcher -5
Capture-Launcher "discover_bottom"
Open-GlobalSearch
Capture-Launcher "search_open"
Type-Text "Sodium"
Capture-Launcher "search_sodium"
Press-Escape
Click-Launcher 120 204
Capture-Launcher "library_top"
Scroll-Launcher -5
Capture-Launcher "library_bottom"
Click-Launcher 120 236
Capture-Launcher "downloads_top"
Scroll-Launcher -5
Capture-Launcher "downloads_bottom"
Click-Launcher 120 268
Capture-Launcher "essentials_friends_top"
Scroll-Launcher -5
Capture-Launcher "essentials_friends_bottom"
Click-Launcher 120 300
Capture-Launcher "servers_top"
Scroll-Launcher -5
Capture-Launcher "servers_bottom"
Click-Launcher 120 332
Capture-Launcher "settings_top"
Scroll-Launcher -5
Capture-Launcher "settings_bottom"
Write-Output "NATIVE_QA_COMPLETE"
