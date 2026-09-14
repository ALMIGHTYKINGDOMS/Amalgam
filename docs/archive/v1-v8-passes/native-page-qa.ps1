$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class AmalgamPageQa {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT point);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, uint nFlags);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, int data, UIntPtr extra);
    public const int SW_RESTORE = 9;
    public const uint MOUSE_LEFT_DOWN = 0x0002;
    public const uint MOUSE_LEFT_UP = 0x0004;
    public const uint MOUSE_WHEEL = 0x0800;
}
"@

$exe = (Resolve-Path "cpp/build-vs/amalgam_launcher.exe").Path
$out = (Resolve-Path "cpp/build-vs").Path + "\screenshots\native-page-qa"
New-Item -ItemType Directory -Path $out -Force | Out-Null

function Stop-FreshLaunchers {
    Get-Process -Name amalgam_launcher -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq $exe } |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
}
function Get-LauncherProcess {
    for ($i = 0; $i -lt 40; $i++) {
        $p = Get-Process -Name amalgam_launcher -ErrorAction SilentlyContinue |
            Where-Object { $_.Path -eq $exe } | Select-Object -First 1
        if ($p -and $p.MainWindowHandle -ne 0) { return $p }
        Start-Sleep -Milliseconds 250
    }
    throw "Fresh launcher process did not expose a native window: $exe"
}
function Start-FreshLauncher {
    Stop-FreshLaunchers
    Start-Process -FilePath $exe -WorkingDirectory ((Resolve-Path "cpp/build-vs").Path) | Out-Null
    $p = Get-LauncherProcess
    Start-Sleep -Seconds 1
    return $p
}
function Get-ClientBounds($p) {
    $r = New-Object AmalgamPageQa+RECT
    [AmalgamPageQa]::GetClientRect($p.MainWindowHandle, [ref]$r) | Out-Null
    $pt = New-Object AmalgamPageQa+POINT
    [AmalgamPageQa]::ClientToScreen($p.MainWindowHandle, [ref]$pt) | Out-Null
    return @{ Left=$pt.X; Top=$pt.Y; Width=$r.Right-$r.Left; Height=$r.Bottom-$r.Top }
}
function Focus-Launcher($p) {
    [AmalgamPageQa]::ShowWindowAsync($p.MainWindowHandle, [AmalgamPageQa]::SW_RESTORE) | Out-Null
    [AmalgamPageQa]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
    Start-Sleep -Milliseconds 250
}
function Capture-Launcher([string]$name) {
    $p = Get-LauncherProcess
    Focus-Launcher $p
    $b = Get-ClientBounds $p
    $bmp = New-Object System.Drawing.Bitmap($b.Width, $b.Height)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    $printed = [AmalgamPageQa]::PrintWindow($p.MainWindowHandle, $hdc, 0)
    $g.ReleaseHdc($hdc)
    if (-not $printed) { throw "PrintWindow failed for $name" }
    $path = Join-Path $out ($name + ".png")
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose()
    $bmp.Dispose()
    return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
}
function Click-Launcher([int]$x, [int]$y) {
    $p = Get-LauncherProcess
    Focus-Launcher $p
    $b = Get-ClientBounds $p
    [AmalgamPageQa]::SetCursorPos($b.Left + $x, $b.Top + $y) | Out-Null
    Start-Sleep -Milliseconds 100
    [AmalgamPageQa]::mouse_event([AmalgamPageQa]::MOUSE_LEFT_DOWN, 0, 0, 0, [UIntPtr]::Zero)
    [AmalgamPageQa]::mouse_event([AmalgamPageQa]::MOUSE_LEFT_UP, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 1000
}
function Scroll-Launcher([int]$notches) {
    $p = Get-LauncherProcess
    Focus-Launcher $p
    $b = Get-ClientBounds $p
    [AmalgamPageQa]::SetCursorPos($b.Left + [int]($b.Width * 0.72), $b.Top + [int]($b.Height * 0.76)) | Out-Null
    [AmalgamPageQa]::mouse_event([AmalgamPageQa]::MOUSE_WHEEL, 0, 0, $notches * 120, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 700
}

$pages = @(
    @{ Name = "home"; Y = 140 },
    @{ Name = "discover"; Y = 172 },
    @{ Name = "library"; Y = 204 },
    @{ Name = "downloads"; Y = 236 },
    @{ Name = "essentials"; Y = 268 },
    @{ Name = "servers"; Y = 300 },
    @{ Name = "settings"; Y = 332 }
)

foreach ($page in $pages) {
    Start-FreshLauncher | Out-Null
    if ($page.Y -ne 140) { Click-Launcher 120 $page.Y }
    $topHash = Capture-Launcher ($page.Name + "_top")
    $previousHash = $topHash
    $stable = 0
    $lastName = $page.Name + "_top"
    for ($step = 1; $step -le 5; $step++) {
        Scroll-Launcher -3
        $name = $page.Name + "_middle_" + ("{0:D2}" -f $step)
        $hash = Capture-Launcher $name
        $lastName = $name
        if ($hash -eq $previousHash) { $stable++ } else { $stable = 0 }
        $previousHash = $hash
        if ($stable -ge 2) { break }
    }
    Copy-Item -LiteralPath (Join-Path $out ($lastName + ".png")) -Destination (Join-Path $out ($page.Name + "_bottom.png")) -Force
    Write-Output ("PAGE_QA " + $page.Name + " top=" + $topHash + " bottom=" + $previousHash)
}
Stop-FreshLaunchers
Write-Output "PAGE_QA_COMPLETE"
