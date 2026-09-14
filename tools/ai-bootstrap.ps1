# =============================================================================
# AMALGAM AI BOOTSTRAPPER v1.0.0
# -----------------------------------------------------------------------------
# Manifest-driven downloader: resume, retry, SHA-256 verify, staging, atomic
# install, hardware detection, state tracking.
#
# Modes:
#   (default)      Install all required AI components.
#   -Repair        Verify existing, fetch only missing/corrupt components.
#   -VerifyOnly    Check installed state without downloads.
#   -DetectHardware Print hardware report as JSON and exit.
#   -DryRun        Show planned actions without downloading.
#   -Components    Install only the named components (comma-separated).
#   -Quiet         Suppress progress output (useful inside Inno postinstall).
# =============================================================================

[CmdletBinding()]
param(
    [string]$ManifestPath = '',
    [string]$StatePath = '',
    [string]$ModelsDir = '',
    [string]$StagingDir = '',
    [string]$ComponentsCSV = '',
    [switch]$Repair,
    [switch]$VerifyOnly,
    [switch]$DetectHardware,
    [switch]$DryRun,
    [switch]$Quiet,
    [int]$Retries = 3
)

$ErrorActionPreference = 'Stop'
$script:AI_ROOT = Split-Path -Parent $PSScriptRoot
$script:ProgressIntervalSec = 5
$script:StageBase = $null

function Write-Log($msg) {
    if (-not $script:Quiet) { Write-Host $msg }
}

# ---- Default paths ----------------------------------------------------------
$LocalAppData = [Environment]::GetFolderPath('LocalApplicationData')

if (-not $ManifestPath) { $ManifestPath = Join-Path $script:AI_ROOT 'ai\ai-package-manifest.json' }
# Keep downloaded models and mutable state under LocalAppData by default.
if (-not $StatePath)    { $StatePath    = Join-Path $LocalAppData 'Amalgam\AI\ai-install-state.json' }
if (-not $ModelsDir)    { $ModelsDir    = Join-Path $LocalAppData 'Amalgam\AI\Models' }
if (-not $StagingDir)   { $StagingDir   = Join-Path $LocalAppData 'Amalgam\AI\Downloads' }
$script:StageBase = $StagingDir

$KnowledgeSrc = Join-Path $script:AI_ROOT 'ai\knowledge'
$KnowledgeDst = Join-Path $ModelsDir 'knowledge'
$LicenseSrc   = Join-Path $script:AI_ROOT 'ai\licenses'
$LicenseDst   = Join-Path $ModelsDir 'licenses'

# ---- Manifest load ----------------------------------------------------------
function Get-Manifest {
    if (-not (Test-Path -LiteralPath $ManifestPath)) {
        throw "AI package manifest not found: $ManifestPath"
    }
    $m = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
    if ($m.schema_version -ne 2) { throw "Unsupported AI manifest schema: $($m.schema_version)" }
    if ([string]::IsNullOrWhiteSpace([string]$m.ai_package) -or
        [string]::IsNullOrWhiteSpace([string]$m.runtime_version)) {
        throw "AI package manifest is missing its package or runtime version"
    }
    if (-not $m.components -or @($m.components.PSObject.Properties).Count -eq 0) {
        throw "AI package manifest has no components"
    }
    return $m
}

function Get-SafeRelativePath {
    param(
        [string]$Value,
        [string]$Field
    )
    if ([string]::IsNullOrWhiteSpace($Value)) {
        throw "AI manifest $Field is empty"
    }
    $normalized = $Value.Replace('/', '\\')
    if ([IO.Path]::IsPathRooted($normalized) -or
        $normalized -match '(^|\\)\.\.(\\|$)' -or
        $normalized -match '^[a-zA-Z]:') {
        throw "AI manifest $Field is not a safe relative path: $Value"
    }
    try {
        [IO.Path]::GetFullPath((Join-Path 'C:\\AmalgamModels' $normalized)) | Out-Null
    } catch {
        throw "AI manifest $Field is not a valid path: $Value"
    }
    return $normalized
}

function Assert-Manifest {
    param($Manifest)
    foreach ($property in @($Manifest.components.PSObject.Properties)) {
        $name = [string]$property.Name
        $component = $property.Value
        if ([string]$component.id -ne $name) {
            throw "AI manifest component id mismatch: $name"
        }
        $component.destination = Get-SafeRelativePath ([string]$component.destination) "${name}.destination"
        $component.filename = Get-SafeRelativePath ([string]$component.filename) "${name}.filename"
        $size = 0L
        try { $size = [int64]$component.size_bytes } catch { $size = 0L }
        if ($name -ne 'knowledge' -and $size -le 0) {
            throw "AI manifest component has invalid size: $name"
        }
        $hash = [string]$component.sha256
        if (-not [string]::IsNullOrWhiteSpace($hash) -and $hash -notmatch '^[0-9a-fA-F]{64}$') {
            throw "AI manifest component has invalid SHA-256: $name"
        }
        if ($name -ne 'knowledge' -and [string]::IsNullOrWhiteSpace($hash)) {
            throw "AI manifest component is missing SHA-256: $name"
        }
        $url = [string]$component.download_url
        if ($name -ne 'knowledge' -and [string]::IsNullOrWhiteSpace($url)) {
            throw "AI manifest component is missing download URL: $name"
        }
        if (-not [string]::IsNullOrWhiteSpace($url)) {
            try {
                $uri = [Uri]$url
                if ($uri.Scheme -ne 'https' -or [string]::IsNullOrWhiteSpace($uri.Host)) {
                    throw "not HTTPS"
                }
            } catch {
                throw "AI manifest component URL must use HTTPS: $name"
            }
        }
    }
}

function Get-ComponentPath {
    param($Component)
    $root = [IO.Path]::GetFullPath($ModelsDir)
    $rootPrefix = $root
    if (-not $rootPrefix.EndsWith([IO.Path]::DirectorySeparatorChar)) {
        $rootPrefix += [IO.Path]::DirectorySeparatorChar
    }
    $relative = Join-Path ([string]$Component.destination) ([string]$Component.filename)
    $path = [IO.Path]::GetFullPath((Join-Path $root $relative))
    if (-not $path.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "AI component path escapes the model directory"
    }
    return $path
}

function Test-ComponentFile {
    param($Component, [string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    try {
        $item = Get-Item -LiteralPath $Path
        if ([int64]$Component.size_bytes -gt 0 -and $item.Length -ne [int64]$Component.size_bytes) {
            return $false
        }
        if (-not [string]::IsNullOrWhiteSpace([string]$Component.sha256)) {
            return ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -eq ([string]$Component.sha256).ToUpperInvariant())
        }
        return $true
    } catch {
        return $false
    }
}

# ---- State persistence ------------------------------------------------------
function Get-State {
    if (Test-Path -LiteralPath $StatePath) {
        try { return Get-Content -LiteralPath $StatePath -Raw | ConvertFrom-Json }
        catch { return New-Object PSObject }
    }
    return New-Object PSObject
}

function Write-State($state) {
    $dir = Split-Path -Parent $StatePath
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    $txt = $state | ConvertTo-Json -Depth 8 -Compress
    $tmpFile = "$StatePath.tmp-$PID-$([guid]::NewGuid().ToString('N'))"
    # BOM-less UTF-8 so any JSON consumer (launcher, Node, python) can parse it.
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($tmpFile, $txt, $utf8NoBom)
    # Replace the state file only after the complete JSON has reached disk.
    # A unique sibling prevents two installer/repair processes from sharing a
    # temporary filename and publishing a truncated state document.
    if (Test-Path -LiteralPath $StatePath) {
        [System.IO.File]::Replace($tmpFile, $StatePath, $null, $true)
    } else {
        [System.IO.File]::Move($tmpFile, $StatePath)
    }
}

# ---- Hardware detection -----------------------------------------------------
function Get-HardwareReport {
    $cpu   = 'Unknown'
    $gpu   = 'Unknown'
    $vram  = 0
    $ram   = 0
    $os    = 'Unknown'
    try { $cs = Get-CimInstance Win32_ComputerSystem -ErrorAction SilentlyContinue; if ($cs) { if ($cs.ProcessorName) { $cpu = [string]$cs.ProcessorName }; if ($cs.TotalPhysicalMemory) { $ram = [math]::Round($cs.TotalPhysicalMemory / 1MB) } } } catch {}
    try { $g = Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue | Select-Object -First 1; if ($g) { $gpu = $g.Name; $vram = [math]::Round($g.AdapterRAM / 1MB) } } catch {}
    try { $os = (Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue).Caption } catch {}
    $cores = [Environment]::ProcessorCount
    $vulkan = $false
    try { if (Get-Command vulkaninfo -ErrorAction SilentlyContinue) { $vulkan = $true } } catch {}
    if (-not $vulkan -and (Test-Path "$env:WINDIR\System32\vulkan-1.dll")) { $vulkan = $true }
    $mode = 'CPU'
    if ($vram -ge 12288) { $mode = 'GPU' }
    elseif ($vram -ge 6144) { $mode = 'Balanced' }
    elseif ($vram -ge 2048) { $mode = 'LowMemory' }
    return [PSCustomObject]@{ cpu = $cpu; cores = $cores; ram_mb = $ram; gpu = $gpu; vram_mb = $vram; vulkan = $vulkan; os = $os; mode = $mode }
}

# ---- Disk free bytes --------------------------------------------------------
function Get-FreeBytes($path) {
    try {
        # The staging directory is normally created by the first download, so
        # Resolve-Path is not usable for the preflight check. Resolve the drive
        # from the requested path without creating any user directories.
        $full = [System.IO.Path]::GetFullPath($path)
        $drive = [System.IO.Path]::GetPathRoot($full)
        if ([string]::IsNullOrWhiteSpace($drive)) { return -1 }
        return (New-Object System.IO.DriveInfo($drive)).AvailableFreeSpace
    } catch {
        return -1
    }
}

# ---- Format bytes / time ----------------------------------------------------
function Format-Bytes($b) {
    if ($b -ge 1GB) { return '{0:N1} GB' -f ($b / 1GB) }
    if ($b -ge 1MB) { return '{0:N0} MB' -f ($b / 1MB) }
    return '{0:N0} B' -f $b
}

# ---- Core: verified, resumable download per component -----------------------
function Get-ComponentFile {
    param(
        [string]$Url,
        [string]$Dest,
        [int64]$ExpectedSize,
        [string]$ExpectedHash,
        [string]$DisplayName
    )
    $part = "$Dest.part"
    $destDir = Split-Path -Parent $Dest
    New-Item -ItemType Directory -Path $destDir -Force | Out-Null

    # Already-verified installed file
    if (Test-Path -LiteralPath $Dest) {
        $sz = (Get-Item -LiteralPath $Dest).Length
        if ($ExpectedHash) {
            $h = (Get-FileHash -LiteralPath $Dest -Algorithm SHA256).Hash
            if ($h -eq $ExpectedHash.ToUpper()) {
                Write-Log ("  [SKIP] {0} already installed and verified." -f $DisplayName)
                return $true
            }
        } elseif ($ExpectedSize -gt 0 -and $sz -eq $ExpectedSize) {
            Write-Log ("  [SKIP] {0} already installed (size ok)." -f $DisplayName)
            return $true
        }
        # Leave an invalid existing file in place until a verified replacement
        # is ready. A failed repair must never destroy the previous artifact.
    }

    # Resume from partial
    $offset = 0
    if (Test-Path -LiteralPath $part) {
        $offset = (Get-Item -LiteralPath $part).Length
        if ($ExpectedSize -gt 0 -and $offset -ge $ExpectedSize) {
            Remove-Item -LiteralPath $part -Force
            $offset = 0
        }
    }
    $resumeMsg = if ($offset -gt 0) { ("resuming at {0}" -f (Format-Bytes $offset)) } else { "downloading" }

    for ($attempt = 0; $attempt -le $Retries; $attempt++) {
        try {
            $resumeMsg = if ($offset -gt 0) { ("resuming at {0}" -f (Format-Bytes $offset)) } else { "downloading" }
            Write-Log ("  [{0}] {1}" -f $resumeMsg, $DisplayName)
            $req = [System.Net.HttpWebRequest]::Create($Url)
            $req.UserAgent = 'Amalgam/1.0.0 (ai-bootstrap)'
            $req.Timeout = 120000
            if ($offset -gt 0) { $req.AddRange($offset) }
            $resp = $req.GetResponse()
            # A Range request must be answered with 206 Partial Content. If a
            # mirror ignores Range and returns the entire file, appending it to
            # the .part file would corrupt the download forever. Restart this
            # component cleanly instead.
            if ($offset -gt 0 -and [int]$resp.StatusCode -ne 206) {
                $resp.Dispose()
                if (Test-Path -LiteralPath $part) { Remove-Item -LiteralPath $part -Force }
                $offset = 0
                continue
            }
            $inStream = $resp.GetResponseStream()

            try {
                $fs = [System.IO.File]::Open($part, [System.IO.FileMode]::Append, [System.IO.FileAccess]::Write)
            } catch {
                # If file locked, start fresh
                $fs = [System.IO.File]::Open($part, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
                $offset = 0
            }
            $buffer = New-Object byte[] 262144
            $lastReport = Get-Date
            try {
                while (($read = $inStream.Read($buffer, 0, $buffer.Length)) -gt 0) {
                    $fs.Write($buffer, 0, $read)
                    $offset += $read
                    if ((Get-Date) - $lastReport -gt [TimeSpan]::FromSeconds($script:ProgressIntervalSec)) {
                        Write-Log ("    {0}" -f (Format-Bytes $offset))
                        $lastReport = Get-Date
                    }
                }
            } finally { $fs.Dispose() }
            $inStream.Dispose()
            $resp.Dispose()

            $finalSize = (Get-Item -LiteralPath $part).Length
            if ($ExpectedSize -gt 0 -and $finalSize -ne $ExpectedSize) {
                throw ("Size mismatch: expected {0}, got {1}" -f $ExpectedSize, $finalSize)
            }

            if ($ExpectedHash) {
                $actualHash = (Get-FileHash -LiteralPath $part -Algorithm SHA256).Hash
                if ($actualHash -ne $ExpectedHash.ToUpper()) {
                    Remove-Item -LiteralPath $part -Force
                    throw ("SHA-256 mismatch. Expected {0}" -f $ExpectedHash)
                }
                Write-Log ("    SHA-256 verified.")
            }

            # Atomic activation
            Move-Item -LiteralPath $part -Destination $Dest -Force
            Write-Log ("    Installed {0}" -f (Format-Bytes $finalSize))
            return $true

        } catch {
            $status = 0
            if ($_.Exception -is [Net.WebException] -and $_.Exception.Response) {
                $status = [int]$_.Exception.Response.StatusCode
            }
            # Permanent HTTP failures should be actionable immediately rather
            # than wasting several exponential retries on a missing component.
            if ($status -in @(400, 401, 403, 404, 410)) {
                throw ("Download failed with HTTP {0}: {1}" -f $status, $DisplayName)
            }
            if ($attempt -ge $Retries) { throw }
            $wait = [math]::Min(30, [math]::Pow(2, $attempt))
            Write-Log ("  Retry {0}/{1} in {2}s: {3}" -f ($attempt + 1), $Retries, $wait, $_.Exception.Message)
            Start-Sleep -Seconds $wait
            if (Test-Path -LiteralPath $part) { $offset = (Get-Item -LiteralPath $part).Length }
        }
    }
    return $false
}

# ---- Knowledge + licenses install (tiny, copied from repo) ------------------
function Install-Knowledge {
    if (Test-Path -LiteralPath $KnowledgeSrc) {
        New-Item -ItemType Directory -Path $KnowledgeDst -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $KnowledgeSrc '*') -Destination $KnowledgeDst -Recurse -Force
    }
    if (Test-Path -LiteralPath $LicenseSrc) {
        New-Item -ItemType Directory -Path $LicenseDst -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $LicenseSrc '*') -Destination $LicenseDst -Recurse -Force
    }
}

# -----------------------------------------------------------------------------
# MAIN
# -----------------------------------------------------------------------------
try {
    if ($DetectHardware) {
        $hwOut = Get-HardwareReport | ConvertTo-Json -Depth 4
        if (-not $Quiet) { $hwOut = ($hwOut | ConvertFrom-Json | ConvertTo-Json -Depth 4) }  # pretty in interactive
        Write-Host $hwOut
        exit 0
    }

    if ($Retries -lt 0 -or $Retries -gt 10) { throw "Retries must be between 0 and 10" }
    $manifest = Get-Manifest
    Assert-Manifest $manifest
    $state    = Get-State
    $hw       = Get-HardwareReport

    Write-Log ('Amalgam AI Bootstrapper v1.0.0 - package: {0}' -f $manifest.ai_package)
    Write-Log ('  CPU: {0} ({1} cores)' -f $hw.cpu, $hw.cores)
    Write-Log ('  RAM: {0} GB' -f [math]::Round($hw.ram_mb / 1024, 1))
    Write-Log ('  GPU: {0} (VRAM {1} GB)' -f $hw.gpu, [math]::Round($hw.vram_mb / 1024, 1))
    Write-Log ('  Vulkan: {0}  |  Mode: {1}' -f $hw.vulkan, $hw.mode)
    Write-Log ''

    # Disk check
    $need = [int64]$manifest.total_download_bytes + [int64]$manifest.total_installed_bytes + (4GB)
    $free = Get-FreeBytes $script:StageBase
    if ($free -gt 0 -and $free -lt $need) {
        Write-Log ('WARNING: Available disk {0}, required ~{1}.' -f (Format-Bytes $free), (Format-Bytes $need))
    }

    # Determine components list
    $compNames = if ($ComponentsCSV) { @($ComponentsCSV -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ }) }
                else { @($manifest.components.PSObject.Properties.Name) }
    foreach ($name in $compNames) {
        if (-not $manifest.components.PSObject.Properties[$name]) {
            throw "Unknown AI component: $name"
        }
    }

    # Verify-only mode
    if ($VerifyOnly) {
        $problems = @()
        foreach ($name in $compNames) {
            $c = $manifest.components.$name
            $file = Get-ComponentPath $c
            $ok = Test-ComponentFile $c $file
            Write-Log ('  [{0}] {1}' -f $(if ($ok) { 'OK ' } else { 'MIS' }), $name)
            if (-not $ok) { $problems += $name }
        }
        if ($problems.Count -gt 0) {
            Write-Log ('AI verification: {0} component(s) need attention.' -f $problems.Count)
            exit 2
        }
        Write-Log 'AI verification: ALL COMPONENTS PRESENT.'
        exit 0
    }

    # Dry-run
    if ($DryRun) {
        Write-Log 'DRY RUN -- planned actions:'
        foreach ($name in $compNames) {
            $c = $manifest.components.$name
            $file = Get-ComponentPath $c
            $action = if (Test-ComponentFile $c $file) { 'skip (verified)' } else { 'download' }
            Write-Log ('  [{0}] {1} ({2}) -> {3}' -f $action, $name, (Format-Bytes ([int64]$c.size_bytes)), $file)
        }
        exit 0
    }

    # ---- Perform installation / repair ------------------------------------------
    $failures = @()
    $installedCount = 0
    foreach ($name in $compNames) {
        $c = $manifest.components.$name
        if (-not $c) { Write-Log ("  Unknown component: {0}" -f $name); $failures += $name; continue }

        # Knowledge is a copy-from-repo component
        if ($name -eq 'knowledge') {
            Install-Knowledge
            $componentState = [PSCustomObject]@{ status = 'installed'; verified_at = (Get-Date).ToString('o') }
            $state | Add-Member -NotePropertyName $name -NotePropertyValue $componentState -Force
            $installedCount++
            continue
        }

        if (-not $c.download_url) { continue }

        $file = Get-ComponentPath $c
        $dest = Split-Path -Parent $file

        # Quick skip if already good
        $alreadyGood = Test-ComponentFile $c $file
        if ($alreadyGood) {
            Write-Log ("  [OK] {0}" -f $name)
            $componentState = [PSCustomObject]@{ status = 'installed'; verified_at = (Get-Date).ToString('o') }
            $state | Add-Member -NotePropertyName $name -NotePropertyValue $componentState -Force
            $installedCount++
            continue
        }

        try {
            $downloadSuccess = Get-ComponentFile -Url $c.download_url -Dest $file `
                -ExpectedSize ([int64]$c.size_bytes) -ExpectedHash $c.sha256 `
                -DisplayName $c.name
            if ($downloadSuccess) {
                $componentState = [PSCustomObject]@{ status = 'installed'; verified_at = (Get-Date).ToString('o') }
                $state | Add-Member -NotePropertyName $name -NotePropertyValue $componentState -Force
                $installedCount++
            }
        } catch {
            Write-Log ("  [FAILED] {0} : {1}" -f $name, $_.Exception.Message)
            $failures += $name
        }
    }

    $state | Add-Member -NotePropertyName 'installed_at' -NotePropertyValue (Get-Date).ToString('o') -Force
    $state | Add-Member -NotePropertyName 'package' -NotePropertyValue $manifest.ai_package -Force
    $state | Add-Member -NotePropertyName 'hardware' -NotePropertyValue $hw -Force
    Write-State $state

    Write-Log ''
    if ($failures.Count -eq 0) {
        Write-Log ('Amalgam AI installation complete. {0} component(s) installed.' -f $installedCount)
        exit 0
    } else {
        Write-Log ('Amalgam AI finished with {0} failed: {1}' -f $failures.Count, ($failures -join ', '))
        Write-Log ('Run: tools\ai-bootstrap.cmd -Repair to retry failed components.')
        exit 1
    }
} catch {
    Write-Log ("ERROR: {0}" -f $_.Exception.Message)
    exit 1
}