<#
.SYNOPSIS
Creates a non-destructive visual-review gallery for a launcher capture run.

.DESCRIPTION
Reads the source visual-capture ledger, validates each referenced PNG, and
writes a new review directory containing high-resolution contact sheets, a
Markdown audit index, an HTML gallery, and a machine-readable review ledger.
The source capture run is read only: no PNG, ledger, stdout/stderr log, or
manifest copy is replaced.

.EXAMPLE
& .\tools\review-launcher-visual-qa.ps1 `
  -RunDirectory "C:\evidence\launcher-visual-qa-20260923T..."

.EXAMPLE
& .\tools\review-launcher-visual-qa.ps1 `
  -LedgerPath "C:\evidence\launcher-visual-qa-20260923T...\visual-capture-ledger.json" `
  -OutputRoot "C:\evidence\reviews" `
  -GroupBy Surface -ThumbnailWidth 720 -Columns 2 -RowsPerSheet 2
#>
[CmdletBinding(DefaultParameterSetName = "RunDirectory")]
param(
    [Parameter(Mandatory = $true, ParameterSetName = "RunDirectory")]
    [ValidateNotNullOrEmpty()]
    [string]$RunDirectory,

    [Parameter(Mandatory = $true, ParameterSetName = "LedgerPath")]
    [ValidateNotNullOrEmpty()]
    [string]$LedgerPath,

    # When omitted, the script uses the immutable manifest copy stored next to
    # the ledger. A separate manifest is useful for legacy capture runs.
    [ValidateNotNullOrEmpty()]
    [string]$ManifestPath,

    # This is a *root*, not an existing review directory. The script always
    # creates a new uniquely named child beneath it and never overwrites prior
    # evidence or a previous review.
    [ValidateNotNullOrEmpty()]
    [string]$OutputRoot,

    [ValidateSet("Auto", "Surface", "RoutePrefix", "Single")]
    [string]$GroupBy = "Auto",

    [ValidateRange(160, 1000)]
    [int]$ThumbnailWidth = 600,

    [ValidateRange(1, 6)]
    [int]$Columns = 2,

    [ValidateRange(1, 8)]
    [int]$RowsPerSheet = 3
)

# Produces a secondary, immutable review presentation for an existing launcher
# visual-capture run. It never changes a source PNG, stdout/stderr file, or
# visual-capture ledger. A contact sheet is an efficient way to find layout
# outliers; inspect the linked original PNGs for text-level decisions.

$ErrorActionPreference = "Stop"

try {
    Add-Type -AssemblyName System.Drawing -ErrorAction Stop
}
catch {
    throw "The visual-review helper needs the Windows System.Drawing assembly: $($_.Exception.Message)"
}

function Write-Utf8NoBom {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Text
    )

    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Text, $encoding)
}

function ConvertTo-Text {
    param([AllowNull()][object]$Value)

    if ($null -eq $Value) { return "" }
    return [string]$Value
}

function ConvertTo-MarkdownCell {
    param([AllowNull()][object]$Value)

    return ((ConvertTo-Text $Value) -replace '[\r\n]+', ' ' -replace '\|', '\\|')
}

function ConvertTo-Html {
    param([AllowNull()][object]$Value)

    return [System.Net.WebUtility]::HtmlEncode((ConvertTo-Text $Value))
}

function ConvertTo-SafeFileStem {
    param(
        [Parameter(Mandatory = $true)][string]$Value,
        [Parameter(Mandatory = $true)][string]$Fallback
    )

    $stem = $Value -replace '[^A-Za-z0-9._-]+', '-'
    $stem = $stem.Trim('-_.')
    if ([string]::IsNullOrWhiteSpace($stem)) { $stem = $Fallback }
    return $stem.Substring(0, [Math]::Min(72, $stem.Length))
}

function Get-RelativePathForLink {
    param(
        [Parameter(Mandatory = $true)][string]$BaseDirectory,
        [Parameter(Mandatory = $true)][string]$TargetPath
    )

    $baseFull = [System.IO.Path]::GetFullPath($BaseDirectory)
    if (-not $baseFull.EndsWith([System.IO.Path]::DirectorySeparatorChar)) {
        $baseFull += [System.IO.Path]::DirectorySeparatorChar
    }
    $baseUri = New-Object System.Uri($baseFull)
    $targetUri = New-Object System.Uri([System.IO.Path]::GetFullPath($TargetPath))
    return $baseUri.MakeRelativeUri($targetUri).ToString()
}

function Get-PngMetadata {
    param([Parameter(Mandatory = $true)][string]$Path)

    $stream = [System.IO.File]::Open(
        $Path,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::Read
    )
    try {
        [byte[]]$header = New-Object byte[] 24
        $read = $stream.Read($header, 0, $header.Length)
        if ($read -ne $header.Length) {
            throw "PNG is shorter than its required header."
        }

        [byte[]]$signature = 137, 80, 78, 71, 13, 10, 26, 10
        for ($index = 0; $index -lt $signature.Length; $index++) {
            if ($header[$index] -ne $signature[$index]) {
                throw "File does not have a PNG signature."
            }
        }
        if ($header[12] -ne 73 -or $header[13] -ne 72 -or
            $header[14] -ne 68 -or $header[15] -ne 82) {
            throw "PNG is missing its IHDR chunk."
        }

        [int64]$width =
            ([int64]$header[16] * 16777216) +
            ([int64]$header[17] * 65536) +
            ([int64]$header[18] * 256) +
            [int64]$header[19]
        [int64]$height =
            ([int64]$header[20] * 16777216) +
            ([int64]$header[21] * 65536) +
            ([int64]$header[22] * 256) +
            [int64]$header[23]
        if ($width -le 0 -or $height -le 0) {
            throw "PNG contains an invalid ${width}x${height} IHDR size."
        }
        return [pscustomobject]@{ Width = $width; Height = $height }
    }
    finally {
        $stream.Dispose()
    }
}

function Get-Sha256File {
    param([Parameter(Mandatory = $true)][string]$Path)

    # Keep review verification independent of PowerShell module auto-loading.
    $stream = [System.IO.File]::Open(
        $Path,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::Read
    )
    $algorithm = [System.Security.Cryptography.SHA256]::Create()
    try {
        $hash = $algorithm.ComputeHash($stream)
        return [System.BitConverter]::ToString($hash).Replace('-', '').ToLowerInvariant()
    }
    finally {
        $algorithm.Dispose()
        $stream.Dispose()
    }
}

function Test-PngCanDecode {
    param([Parameter(Mandatory = $true)][string]$Path)

    $image = $null
    try {
        $image = [System.Drawing.Image]::FromFile($Path)
        if ($image.Width -le 0 -or $image.Height -le 0) {
            throw "GDI+ decoded an invalid image size."
        }
    }
    finally {
        if ($null -ne $image) { $image.Dispose() }
    }
}

function Get-RoutePrefix {
    param(
        [AllowNull()][string]$Route,
        [AllowNull()][string]$CaseId
    )

    $candidate = $Route
    if ([string]::IsNullOrWhiteSpace($candidate)) { $candidate = $CaseId }
    if ([string]::IsNullOrWhiteSpace($candidate)) { return "Unclassified" }
    $prefix = ($candidate -split '[-/@]', 2)[0]
    if ([string]::IsNullOrWhiteSpace($prefix)) { return "Unclassified" }
    return ([Globalization.CultureInfo]::InvariantCulture.TextInfo.ToTitleCase($prefix.ToLowerInvariant()))
}

function Get-ManifestCaseMap {
    param([AllowNull()][string]$Path)

    $map = @{}
    $warnings = New-Object System.Collections.Generic.List[string]
    if ([string]::IsNullOrWhiteSpace($Path)) {
        return [pscustomobject]@{ Cases = $map; Warnings = @($warnings); Source = $null }
    }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        [void]$warnings.Add("Manifest was not found: $Path")
        return [pscustomobject]@{ Cases = $map; Warnings = @($warnings); Source = $null }
    }

    try {
        $manifest = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    }
    catch {
        [void]$warnings.Add("Manifest could not be parsed: $($_.Exception.Message)")
        return [pscustomobject]@{ Cases = $map; Warnings = @($warnings); Source = $Path }
    }

    foreach ($case in @($manifest.cases)) {
        if ($null -eq $case) { continue }
        $id = ConvertTo-Text $case.id
        if ([string]::IsNullOrWhiteSpace($id)) { continue }
        $key = $id.ToLowerInvariant()
        if ($map.ContainsKey($key)) {
            [void]$warnings.Add("Manifest repeats case id '$id'; its first description was retained.")
            continue
        }
        $map[$key] = [pscustomobject]@{
            Surface = ConvertTo-Text $case.surface
            State = ConvertTo-Text $case.state
            Annotation = ConvertTo-Text $case.annotation
        }
    }
    return [pscustomobject]@{ Cases = $map; Warnings = @($warnings); Source = $Path }
}

function Get-SourceImagePath {
    param(
        [Parameter(Mandatory = $true)][string]$LedgerDirectory,
        [AllowNull()][string]$ImageFile
    )

    return Get-RunArtifactPath -LedgerDirectory $LedgerDirectory -FileName $ImageFile
}

function Get-RunArtifactPath {
    param(
        [Parameter(Mandatory = $true)][string]$LedgerDirectory,
        [AllowNull()][string]$FileName
    )

    if ([string]::IsNullOrWhiteSpace($FileName)) { return $null }
    # Source ledgers store evidence artifact filenames only. Refuse a
    # legacy/malformed entry that could point a review at an unrelated file
    # outside its immutable evidence run.
    if ([System.IO.Path]::IsPathRooted($FileName) -or
        $FileName -ne [System.IO.Path]::GetFileName($FileName)) {
        return $null
    }
    return (Join-Path $LedgerDirectory $FileName)
}

function Get-ProvenanceFingerprint {
    param(
        [AllowNull()][string]$Path,
        [AllowNull()][string]$ExpectedSha256,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $expected = ConvertTo-Text $ExpectedSha256
    if ([string]::IsNullOrWhiteSpace($expected)) {
        return [pscustomobject][ordered]@{
            label = $Label
            path = $Path
            expectedSha256 = $null
            actualSha256 = $null
            status = "not-recorded"
            detail = "The source ledger did not record a SHA-256 value."
        }
    }
    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return [pscustomobject][ordered]@{
            label = $Label
            path = $Path
            expectedSha256 = $expected
            actualSha256 = $null
            status = "missing"
            detail = "The file is no longer available at its recorded path."
        }
    }

    try {
        $actual = Get-Sha256File -Path $Path
        $status = if ($actual.Equals($expected, [System.StringComparison]::OrdinalIgnoreCase)) {
            "matches"
        }
        else {
            "mismatch"
        }
        return [pscustomobject][ordered]@{
            label = $Label
            path = $Path
            expectedSha256 = $expected
            actualSha256 = $actual
            status = $status
            detail = $(if ($status -eq "matches") {
                    "The available file matches the SHA-256 captured in the source ledger."
                }
                else {
                    "The available file differs from the SHA-256 captured in the source ledger."
                })
        }
    }
    catch {
        return [pscustomobject][ordered]@{
            label = $Label
            path = $Path
            expectedSha256 = $expected
            actualSha256 = $null
            status = "unreadable"
            detail = "Could not read the file for SHA-256 verification: $($_.Exception.Message)"
        }
    }
}

function Get-LedgerProvenance {
    param(
        [Parameter(Mandatory = $true)][object]$Ledger,
        [Parameter(Mandatory = $true)][string]$LedgerDirectory
    )

    $run = $Ledger.run
    if ($null -eq $run) {
        return [pscustomobject][ordered]@{
            launcherBinary = Get-ProvenanceFingerprint -Path $null -ExpectedSha256 $null -Label "Launcher binary"
            manifestCopy = Get-ProvenanceFingerprint -Path $null -ExpectedSha256 $null -Label "Captured manifest copy"
            fixtureInventory = Get-ProvenanceFingerprint -Path $null -ExpectedSha256 $null -Label "Built-binary fixture inventory"
            caseSelection = Get-ProvenanceFingerprint -Path $null -ExpectedSha256 $null -Label "Case-selection artifact"
            fixtureInventoryGate = [pscustomobject][ordered]@{
                status = "not-recorded"
                detail = "The source ledger has no built-binary fixture inventory metadata."
            }
            configurationIsolation = [pscustomobject][ordered]@{
                status = "not-recorded"
                detail = "The source ledger has no run metadata."
            }
        }
    }

    $launcherPath = ConvertTo-Text $run.launcherPath
    $manifestCopyPath = Get-RunArtifactPath -LedgerDirectory $LedgerDirectory `
        -FileName (ConvertTo-Text $run.manifestCopy)
    $fixtureInventoryPath = Get-RunArtifactPath -LedgerDirectory $LedgerDirectory `
        -FileName (ConvertTo-Text $run.fixtureInventory)
    $selectionPath = Get-RunArtifactPath -LedgerDirectory $LedgerDirectory `
        -FileName (ConvertTo-Text $run.caseSelection)
    $fixtureIsolation = $run.fixtureIsolation
    $configUnchanged = $run.launcherConfigUnchanged
    $isolationStatus = "not-recorded"
    $isolationDetail = "The source ledger predates per-capture isolation reporting."
    if ($null -ne $fixtureIsolation) {
        $allApplied = [bool]$fixtureIsolation.childEnvironmentAppliedForEveryAttempt
        $allRestored = [bool]$fixtureIsolation.childEnvironmentRestoredForEveryAttempt
        $configPassed = $configUnchanged -eq $true
        if ((ConvertTo-Text $fixtureIsolation.outcome) -eq "passed" -and
            $allApplied -and $allRestored -and $configPassed) {
            $isolationStatus = "passed"
            $isolationDetail = "Every attempted capture used and restored its isolated child environment; the launcher configuration fingerprint was unchanged."
        }
        else {
            $isolationStatus = "failed"
            $isolationDetail = "The source ledger reports an incomplete child-environment or launcher-configuration isolation outcome."
        }
    }
    $fixtureInventoryGateStatus = "not-recorded"
    $fixtureInventoryGateDetail = "The source ledger predates fixture-inventory integrity reporting."
    $recordedFixtureIntegrity = ConvertTo-Text $run.fixtureInventoryIntegrity
    if (-not [string]::IsNullOrWhiteSpace($recordedFixtureIntegrity)) {
        if ($recordedFixtureIntegrity -eq "passed") {
            $fixtureInventoryGateStatus = "passed"
            $fixtureInventoryGateDetail = "The capture runner recorded a matching built-binary fixture inventory before opening snapshot windows."
        }
        else {
            $fixtureInventoryGateStatus = "failed"
            $fixtureInventoryGateDetail = "The source ledger reports fixture-inventory integrity '$recordedFixtureIntegrity'."
        }
    }

    return [pscustomobject][ordered]@{
        launcherBinary = Get-ProvenanceFingerprint -Path $launcherPath `
            -ExpectedSha256 (ConvertTo-Text $run.launcherSha256) -Label "Launcher binary"
        manifestCopy = Get-ProvenanceFingerprint -Path $manifestCopyPath `
            -ExpectedSha256 (ConvertTo-Text $(if ($null -ne $run.manifestCopySha256) { $run.manifestCopySha256 } else { $run.manifestSha256 })) `
            -Label "Captured manifest copy"
        fixtureInventory = Get-ProvenanceFingerprint -Path $fixtureInventoryPath `
            -ExpectedSha256 (ConvertTo-Text $run.fixtureInventorySha256) -Label "Built-binary fixture inventory"
        caseSelection = Get-ProvenanceFingerprint -Path $selectionPath `
            -ExpectedSha256 (ConvertTo-Text $run.caseSelectionSha256) -Label "Case-selection artifact"
        fixtureInventoryGate = [pscustomobject][ordered]@{
            status = $fixtureInventoryGateStatus
            detail = $fixtureInventoryGateDetail
            binaryFixtureCount = $run.binaryFixtureCount
            manifestDeclaredFixtureCount = $run.manifestDeclaredFixtureCount
            manifestUniqueFixtureRouteCount = $run.manifestUniqueFixtureRouteCount
        }
        configurationIsolation = [pscustomobject][ordered]@{
            status = $isolationStatus
            detail = $isolationDetail
            launcherConfigUnchanged = $configUnchanged
            recordedOutcome = $(if ($null -eq $fixtureIsolation) { $null } else { $fixtureIsolation.outcome })
            mode = $(if ($null -eq $fixtureIsolation) { $null } else { $fixtureIsolation.mode })
        }
    }
}

function Get-ReviewRecord {
    param(
        [Parameter(Mandatory = $true)][object]$Capture,
        [Parameter(Mandatory = $true)][string]$LedgerDirectory,
        [Parameter(Mandatory = $true)][hashtable]$ManifestCases,
        [Parameter(Mandatory = $true)][string]$Grouping
    )

    $caseId = ConvertTo-Text $Capture.caseId
    $route = ConvertTo-Text $Capture.route
    $scroll = ConvertTo-Text $Capture.scroll
    $ledgerStatus = ConvertTo-Text $Capture.status
    $manifestCase = $null
    $caseKey = $caseId.ToLowerInvariant()
    if (-not [string]::IsNullOrWhiteSpace($caseId) -and $ManifestCases.ContainsKey($caseKey)) {
        $manifestCase = $ManifestCases[$caseKey]
    }

    # Schema-2 ledgers retain their resolved surface/state. Prefer that
    # preserved capture metadata when a manifest copy is unavailable, while a
    # valid manifest can still fill missing legacy fields.
    $surface = ConvertTo-Text $Capture.surface
    $state = ConvertTo-Text $Capture.state
    $annotation = ConvertTo-Text $Capture.annotation
    if ($null -ne $manifestCase) {
        if ([string]::IsNullOrWhiteSpace($surface)) { $surface = ConvertTo-Text $manifestCase.Surface }
        if ([string]::IsNullOrWhiteSpace($state)) { $state = ConvertTo-Text $manifestCase.State }
        if ([string]::IsNullOrWhiteSpace($annotation)) {
            $annotation = ConvertTo-Text $manifestCase.Annotation
        }
    }
    if ([string]::IsNullOrWhiteSpace($state)) { $state = $annotation }
    if ([string]::IsNullOrWhiteSpace($state)) { $state = $route }

    switch ($Grouping) {
        "Single" { $area = "All captures" }
        "RoutePrefix" { $area = Get-RoutePrefix -Route $route -CaseId $caseId }
        "Surface" {
            $area = $surface
            if ([string]::IsNullOrWhiteSpace($area)) { $area = "Unclassified" }
        }
        default {
            $area = $surface
            if ([string]::IsNullOrWhiteSpace($area)) {
                $area = Get-RoutePrefix -Route $route -CaseId $caseId
            }
        }
    }

    $imageFile = ConvertTo-Text $Capture.imageFile
    $imagePath = Get-SourceImagePath -LedgerDirectory $LedgerDirectory -ImageFile $imageFile
    $issues = New-Object System.Collections.Generic.List[string]
    $integrity = "missing"
    $actualWidth = $null
    $actualHeight = $null
    $actualHash = $null
    $canRender = $false

    if ([string]::IsNullOrWhiteSpace($imageFile)) {
        [void]$issues.Add("Ledger does not name an image file.")
    }
    elseif ($null -eq $imagePath) {
        [void]$issues.Add("Ledger image filename is not a safe filename-only value.")
    }
    elseif (-not (Test-Path -LiteralPath $imagePath -PathType Leaf)) {
        [void]$issues.Add("Image is missing.")
    }
    else {
        try {
            $file = Get-Item -LiteralPath $imagePath -ErrorAction Stop
            if ($file.Length -le 0) { throw "Image is empty." }
            $metadata = Get-PngMetadata -Path $imagePath
            $actualWidth = [int64]$metadata.Width
            $actualHeight = [int64]$metadata.Height
            Test-PngCanDecode -Path $imagePath
            $canRender = $true
            $actualHash = Get-Sha256File -Path $imagePath
            $integrity = "valid"
        }
        catch {
            [void]$issues.Add("Image is unreadable: $($_.Exception.Message)")
            $integrity = "invalid"
        }
    }

    $expectedHash = ConvertTo-Text $Capture.sha256
    if ([string]::IsNullOrWhiteSpace($expectedHash) -and $null -ne $Capture.image) {
        $expectedHash = ConvertTo-Text $Capture.image.sha256
    }
    if ($canRender -and -not [string]::IsNullOrWhiteSpace($expectedHash) -and
        -not $actualHash.Equals($expectedHash, [System.StringComparison]::OrdinalIgnoreCase)) {
        [void]$issues.Add("SHA-256 differs from the source ledger.")
        $integrity = "hash-mismatch"
    }

    $recordedWidth = $null
    $recordedHeight = $null
    if ($null -ne $Capture.viewport) {
        $recordedWidth = $Capture.viewport.actualWidth
        $recordedHeight = $Capture.viewport.actualHeight
    }
    if (($null -eq $recordedWidth -or $null -eq $recordedHeight) -and
        $null -ne $Capture.image -and $null -ne $Capture.image.ihdr) {
        $recordedWidth = $Capture.image.ihdr.width
        $recordedHeight = $Capture.image.ihdr.height
    }
    if ($canRender -and $null -ne $recordedWidth -and $null -ne $recordedHeight -and
        (([int64]$recordedWidth -ne $actualWidth) -or ([int64]$recordedHeight -ne $actualHeight))) {
        [void]$issues.Add("PNG dimensions differ from the source ledger (${recordedWidth}x${recordedHeight}).")
        if ($integrity -eq "valid") { $integrity = "dimension-mismatch" }
    }

    return [pscustomobject][ordered]@{
        caseId = $caseId
        route = $route
        surface = $surface
        state = $state
        annotation = $annotation
        scroll = $scroll
        group = $area
        ledgerStatus = $ledgerStatus
        ledgerError = ConvertTo-Text $Capture.error
        imageFile = $imageFile
        imagePath = $imagePath
        sourceImageRelative = ""
        actualWidth = $actualWidth
        actualHeight = $actualHeight
        sha256 = $actualHash
        integrity = $integrity
        canRender = $canRender
        issues = @($issues)
        contactSheet = ""
    }
}

function New-UniqueReviewDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$RunPath,
        [AllowNull()][string]$RequestedOutputRoot
    )

    $runFullPath = [System.IO.Path]::GetFullPath($RunPath)
    $runName = Split-Path -Leaf $runFullPath
    if ([string]::IsNullOrWhiteSpace($RequestedOutputRoot)) {
        $root = Split-Path -Parent $runFullPath
    }
    else {
        $root = [System.IO.Path]::GetFullPath($RequestedOutputRoot)
    }
    $runPrefix = $runFullPath.TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
    if ($root.Equals($runFullPath, [System.StringComparison]::OrdinalIgnoreCase) -or
        $root.StartsWith($runPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "OutputRoot must be outside the source capture run so review output cannot alter its evidence: $root"
    }
    if (-not (Test-Path -LiteralPath $root)) {
        [void][System.IO.Directory]::CreateDirectory($root)
    }
    elseif (-not (Get-Item -LiteralPath $root).PSIsContainer) {
        throw "OutputRoot exists but is not a directory: $root"
    }

    $stamp = [DateTime]::UtcNow.ToString("yyyyMMddTHHmmssfffZ")
    $suffix = [Guid]::NewGuid().ToString("N").Substring(0, 8)
    $reviewDirectory = Join-Path $root ("review-{0}-{1}-{2}" -f `
            (ConvertTo-SafeFileStem -Value $runName -Fallback "capture-run"), $stamp, $suffix)
    if (Test-Path -LiteralPath $reviewDirectory) {
        throw "Refusing to reuse an existing review directory: $reviewDirectory"
    }
    [void][System.IO.Directory]::CreateDirectory($reviewDirectory)
    [void][System.IO.Directory]::CreateDirectory((Join-Path $reviewDirectory "contact-sheets"))
    return $reviewDirectory
}

function Get-IntegrityPalette {
    param([Parameter(Mandatory = $true)][string]$Integrity)

    switch ($Integrity) {
        "valid" { return [System.Drawing.Color]::FromArgb(76, 201, 132) }
        "hash-mismatch" { return [System.Drawing.Color]::FromArgb(255, 181, 71) }
        "dimension-mismatch" { return [System.Drawing.Color]::FromArgb(255, 181, 71) }
        default { return [System.Drawing.Color]::FromArgb(245, 100, 100) }
    }
}

function Draw-ImagePlaceholder {
    param(
        [Parameter(Mandatory = $true)][System.Drawing.Graphics]$Graphics,
        [Parameter(Mandatory = $true)][System.Drawing.Rectangle]$Bounds,
        [Parameter(Mandatory = $true)][string]$Message
    )

    $backgroundBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(49, 29, 35))
    $borderPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(245, 100, 100), 2)
    $font = New-Object System.Drawing.Font("Segoe UI", 16, [System.Drawing.FontStyle]::Bold)
    $textBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 222, 222))
    $format = New-Object System.Drawing.StringFormat
    try {
        $format.Alignment = [System.Drawing.StringAlignment]::Center
        $format.LineAlignment = [System.Drawing.StringAlignment]::Center
        $format.Trimming = [System.Drawing.StringTrimming]::EllipsisWord
        $Graphics.FillRectangle($backgroundBrush, $Bounds)
        $Graphics.DrawRectangle($borderPen, $Bounds)
        # PowerShell can bind a System.Drawing.Rectangle to the PointF overload
        # on some System.Drawing versions. Make the intended layout rectangle
        # explicit so blocked/missing evidence still renders as a visible tile.
        $textBounds = [System.Drawing.RectangleF]::new(
            [single]$Bounds.X, [single]$Bounds.Y, [single]$Bounds.Width, [single]$Bounds.Height)
        $Graphics.DrawString($Message, $font, $textBrush, $textBounds, $format)
    }
    finally {
        $format.Dispose()
        $textBrush.Dispose()
        $font.Dispose()
        $borderPen.Dispose()
        $backgroundBrush.Dispose()
    }
}

function Draw-ReviewTile {
    param(
        [Parameter(Mandatory = $true)][System.Drawing.Graphics]$Graphics,
        [Parameter(Mandatory = $true)][System.Drawing.Rectangle]$Bounds,
        [Parameter(Mandatory = $true)][object]$Record,
        [Parameter(Mandatory = $true)][int]$ImageAreaHeight,
        [Parameter(Mandatory = $true)][int]$Padding
    )

    $panelBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(24, 32, 50))
    $borderPen = New-Object System.Drawing.Pen((Get-IntegrityPalette -Integrity $Record.integrity), 2)
    $titleFont = New-Object System.Drawing.Font("Segoe UI", 15, [System.Drawing.FontStyle]::Bold)
    $bodyFont = New-Object System.Drawing.Font("Segoe UI", 12, [System.Drawing.FontStyle]::Regular)
    $mutedFont = New-Object System.Drawing.Font("Segoe UI", 10, [System.Drawing.FontStyle]::Regular)
    $primaryBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(242, 245, 255))
    $mutedBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(171, 185, 211))
    $warningBrush = New-Object System.Drawing.SolidBrush((Get-IntegrityPalette -Integrity $Record.integrity))
    $captionFormat = New-Object System.Drawing.StringFormat
    try {
        $Graphics.FillRectangle($panelBrush, $Bounds)
        $Graphics.DrawRectangle($borderPen, $Bounds)

        $imageBounds = [System.Drawing.Rectangle]::new(
            ($Bounds.X + $Padding),
            ($Bounds.Y + $Padding),
            ($Bounds.Width - ($Padding * 2)),
            $ImageAreaHeight
        )
        if ($Record.canRender -and -not [string]::IsNullOrWhiteSpace($Record.imagePath)) {
            $source = $null
            try {
                $source = [System.Drawing.Image]::FromFile($Record.imagePath)
                $scale = [Math]::Min(
                    ($imageBounds.Width / [double]$source.Width),
                    ($imageBounds.Height / [double]$source.Height)
                )
                $drawWidth = [int][Math]::Max(1, [Math]::Floor($source.Width * $scale))
                $drawHeight = [int][Math]::Max(1, [Math]::Floor($source.Height * $scale))
                $drawX = $imageBounds.X + [int][Math]::Floor(($imageBounds.Width - $drawWidth) / 2)
                $drawY = $imageBounds.Y + [int][Math]::Floor(($imageBounds.Height - $drawHeight) / 2)
                $Graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                $Graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
                $Graphics.DrawImage($source, [System.Drawing.Rectangle]::new($drawX, $drawY, $drawWidth, $drawHeight))
                $imageOutline = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(72, 91, 127), 1)
                try { $Graphics.DrawRectangle($imageOutline, $drawX, $drawY, $drawWidth - 1, $drawHeight - 1) }
                finally { $imageOutline.Dispose() }
            }
            catch {
                Draw-ImagePlaceholder -Graphics $Graphics -Bounds $imageBounds -Message "Image could not be decoded"
            }
            finally {
                if ($null -ne $source) { $source.Dispose() }
            }
        }
        else {
            $reason = "Missing or invalid evidence"
            if (@($Record.issues).Count -gt 0) { $reason = $Record.issues[0] }
            Draw-ImagePlaceholder -Graphics $Graphics -Bounds $imageBounds -Message $reason
        }

        $captionY = $imageBounds.Bottom + $Padding
        $captionWidth = $Bounds.Width - ($Padding * 2)
        $captionFormat.Trimming = [System.Drawing.StringTrimming]::EllipsisCharacter
        $captionFormat.FormatFlags = [System.Drawing.StringFormatFlags]::NoWrap

        $title = "{0} | {1} | {2}" -f $Record.caseId, $Record.scroll, $Record.ledgerStatus
        $Graphics.DrawString($title, $titleFont, $primaryBrush,
            [System.Drawing.RectangleF]::new($Bounds.X + $Padding, $captionY, $captionWidth, 24), $captionFormat)
        $captionY += 25
        $route = $Record.route
        if ([string]::IsNullOrWhiteSpace($route)) { $route = "(route unavailable)" }
        $Graphics.DrawString($route, $bodyFont, $mutedBrush,
            [System.Drawing.RectangleF]::new($Bounds.X + $Padding, $captionY, $captionWidth, 20), $captionFormat)
        $captionY += 21
        $state = $Record.state
        if ([string]::IsNullOrWhiteSpace($state)) { $state = "(state unavailable)" }
        $Graphics.DrawString($state, $mutedFont, $mutedBrush,
            [System.Drawing.RectangleF]::new($Bounds.X + $Padding, $captionY, $captionWidth, 18), $captionFormat)
        $captionY += 19
        $integrityText = "Evidence: {0}" -f $Record.integrity
        if (@($Record.issues).Count -gt 0) { $integrityText += " | " + $Record.issues[0] }
        $Graphics.DrawString($integrityText, $mutedFont, $warningBrush,
            [System.Drawing.RectangleF]::new($Bounds.X + $Padding, $captionY, $captionWidth, 18), $captionFormat)
    }
    finally {
        $captionFormat.Dispose()
        $warningBrush.Dispose()
        $mutedBrush.Dispose()
        $primaryBrush.Dispose()
        $mutedFont.Dispose()
        $bodyFont.Dispose()
        $titleFont.Dispose()
        $borderPen.Dispose()
        $panelBrush.Dispose()
    }
}

function Write-ContactSheet {
    param(
        [Parameter(Mandatory = $true)][object[]]$Records,
        [Parameter(Mandatory = $true)][string]$Group,
        [Parameter(Mandatory = $true)][int]$SheetIndex,
        [Parameter(Mandatory = $true)][int]$SheetCount,
        [Parameter(Mandatory = $true)][string]$OutputPath,
        [Parameter(Mandatory = $true)][int]$ThumbWidth,
        [Parameter(Mandatory = $true)][int]$ColumnCount
    )

    $padding = 14
    $headerHeight = 84
    $imageHeight = [int][Math]::Ceiling($ThumbWidth * 0.64)
    $captionHeight = 95
    $tileWidth = $ThumbWidth + ($padding * 2)
    $tileHeight = $imageHeight + $captionHeight + ($padding * 2)
    # Avoid an empty half-sheet for small manifest surfaces while retaining the
    # requested column limit for larger batches.
    $effectiveColumns = [Math]::Min($ColumnCount, $Records.Count)
    $rowCount = [int][Math]::Ceiling($Records.Count / [double]$effectiveColumns)
    $canvasWidth = ($effectiveColumns * $tileWidth) + (($effectiveColumns + 1) * $padding)
    $canvasHeight = $headerHeight + ($rowCount * $tileHeight) + (($rowCount + 1) * $padding)

    $bitmap = [System.Drawing.Bitmap]::new($canvasWidth, $canvasHeight, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $headerFont = New-Object System.Drawing.Font("Segoe UI", 20, [System.Drawing.FontStyle]::Bold)
    $subheaderFont = New-Object System.Drawing.Font("Segoe UI", 12, [System.Drawing.FontStyle]::Regular)
    $headerBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(244, 247, 255))
    $subheaderBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(178, 190, 220))
    try {
        $graphics.Clear([System.Drawing.Color]::FromArgb(10, 15, 27))
        $graphics.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::ClearTypeGridFit
        $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
        $header = "Launcher visual evidence | {0}" -f $Group
        $subheader = "{0} capture(s) | sheet {1} of {2} | click through to original PNGs for text-level review" -f `
            $Records.Count, $SheetIndex, $SheetCount
        $graphics.DrawString($header, $headerFont, $headerBrush, $padding, 10)
        $graphics.DrawString($subheader, $subheaderFont, $subheaderBrush, $padding, 45)

        for ($index = 0; $index -lt $Records.Count; $index++) {
            $column = $index % $effectiveColumns
            $row = [int][Math]::Floor($index / [double]$effectiveColumns)
            $x = $padding + ($column * ($tileWidth + $padding))
            $y = $headerHeight + $padding + ($row * ($tileHeight + $padding))
            Draw-ReviewTile -Graphics $graphics `
                -Bounds ([System.Drawing.Rectangle]::new($x, $y, $tileWidth, $tileHeight)) `
                -Record $Records[$index] `
                -ImageAreaHeight $imageHeight `
                -Padding $padding
        }
        $bitmap.Save($OutputPath, [System.Drawing.Imaging.ImageFormat]::Png)
    }
    finally {
        $subheaderBrush.Dispose()
        $headerBrush.Dispose()
        $subheaderFont.Dispose()
        $headerFont.Dispose()
        $graphics.Dispose()
        $bitmap.Dispose()
    }
}

function Write-MarkdownIndex {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][object]$Review,
        [Parameter(Mandatory = $true)][object[]]$Records,
        [Parameter(Mandatory = $true)][object[]]$Groups
    )

    $lines = New-Object System.Collections.Generic.List[string]
    [void]$lines.Add("# Launcher visual-evidence review")
    [void]$lines.Add("")
    [void]$lines.Add("This is a non-destructive review presentation generated from the source capture ledger. It does not replace source evidence. Contact sheets help find visual outliers; open the linked original PNGs before making text-level or interaction judgments.")
    [void]$lines.Add("")
    [void]$lines.Add("- **Generated (UTC):** $($Review.generatedUtc)")
    [void]$lines.Add(("- **Source ledger:** " + [char]96 + $Review.sourceLedger + [char]96))
    [void]$lines.Add("- **Manifest mapping:** $($Review.manifestSource)")
    [void]$lines.Add("- **Grouping:** $($Review.groupBy)")
    [void]$lines.Add("- **Contact-sheet layout:** $($Review.columns) column(s), $($Review.rowsPerSheet) row(s) per sheet, $($Review.thumbnailWidth)px image width")
    [void]$lines.Add("- **Ledger outcome:** $($Review.ledgerPassed) passed, $($Review.ledgerFailed) failed, $($Review.ledgerBlocked) blocked")
    [void]$lines.Add("- **Evidence integrity:** $($Review.validImages) valid, $($Review.hashMismatchImages) hash mismatch, $($Review.dimensionMismatchImages) dimension mismatch, $($Review.invalidImages) invalid, $($Review.missingImages) missing")
    [void]$lines.Add("")
    [void]$lines.Add("## Captured-run provenance and isolation")
    [void]$lines.Add("")
    foreach ($item in @($Review.provenance.launcherBinary, $Review.provenance.manifestCopy,
                         $Review.provenance.fixtureInventory, $Review.provenance.caseSelection)) {
        if ($null -eq $item) { continue }
        $expected = if ([string]::IsNullOrWhiteSpace((ConvertTo-Text $item.expectedSha256))) {
            "not recorded"
        }
        else {
            [char]96 + (ConvertTo-Text $item.expectedSha256) + [char]96
        }
        $actual = if ([string]::IsNullOrWhiteSpace((ConvertTo-Text $item.actualSha256))) {
            "not available"
        }
        else {
            [char]96 + (ConvertTo-Text $item.actualSha256) + [char]96
        }
        [void]$lines.Add(("- **{0}:** {1}; expected SHA-256 {2}; currently available SHA-256 {3}. {4}" -f `
                (ConvertTo-MarkdownCell $item.label), (ConvertTo-MarkdownCell $item.status), $expected, $actual,
                (ConvertTo-MarkdownCell $item.detail)))
    }
    $fixtureInventoryGate = $Review.provenance.fixtureInventoryGate
    if ($null -ne $fixtureInventoryGate) {
        [void]$lines.Add(('- **Fixture-inventory gate:** {0}. {1}' -f `
                (ConvertTo-MarkdownCell $fixtureInventoryGate.status),
                (ConvertTo-MarkdownCell $fixtureInventoryGate.detail)))
    }
    $isolation = $Review.provenance.configurationIsolation
    if ($null -ne $isolation) {
        [void]$lines.Add(("- **Fixture/configuration isolation:** {0}. {1}" -f `
                (ConvertTo-MarkdownCell $isolation.status), (ConvertTo-MarkdownCell $isolation.detail)))
    }
    [void]$lines.Add("- **Per-capture commands, exit codes, PNG IHDR dimensions, and stdout/stderr fingerprints:** retained in the linked source capture ledger.")
    [void]$lines.Add("")
    [void]$lines.Add("## Area batches")
    [void]$lines.Add("")
    [void]$lines.Add("| Area / manifest surface | Captures | Valid | Needs attention | Contact sheets |")
    [void]$lines.Add("| --- | ---: | ---: | ---: | --- |")
    foreach ($group in $Groups) {
        $sheets = @($group.sheets | ForEach-Object { "[$([System.IO.Path]::GetFileName($_))]($(Get-RelativePathForLink -BaseDirectory (Split-Path -Parent $Path) -TargetPath $_))" }) -join "<br>"
        [void]$lines.Add(("| {0} | {1} | {2} | {3} | {4} |" -f `
                (ConvertTo-MarkdownCell $group.name), $group.count, $group.valid, $group.attention, $sheets))
    }
    [void]$lines.Add("")
    [void]$lines.Add("## Capture review ledger")
    [void]$lines.Add("")
    [void]$lines.Add("| Case | Surface / state | Scroll | Capture result | Evidence integrity | Original image | Notes |")
    [void]$lines.Add("| --- | --- | --- | --- | --- | --- | --- |")
    foreach ($record in $Records) {
        $surfaceAndState = $record.surface
        if ([string]::IsNullOrWhiteSpace($surfaceAndState)) { $surfaceAndState = $record.group }
        if (-not [string]::IsNullOrWhiteSpace($record.state)) { $surfaceAndState += " - " + $record.state }
        $image = "missing"
        if ($record.canRender -and -not [string]::IsNullOrWhiteSpace($record.imagePath)) {
            $image = "[$($record.imageFile)]($(Get-RelativePathForLink -BaseDirectory (Split-Path -Parent $Path) -TargetPath $record.imagePath))"
        }
        $notes = @($record.issues) -join " "
        if ([string]::IsNullOrWhiteSpace($notes)) { $notes = $record.ledgerError }
        [void]$lines.Add(("| {0} | {1} | {2} | {3} | {4} | {5} | {6} |" -f `
                (ConvertTo-MarkdownCell $record.caseId),
                (ConvertTo-MarkdownCell $surfaceAndState),
                (ConvertTo-MarkdownCell $record.scroll),
                (ConvertTo-MarkdownCell $record.ledgerStatus),
                (ConvertTo-MarkdownCell $record.integrity),
                $image,
                (ConvertTo-MarkdownCell $notes)))
    }
    if (@($Review.warnings).Count -gt 0) {
        [void]$lines.Add("")
        [void]$lines.Add("## Mapping warnings")
        [void]$lines.Add("")
        foreach ($warning in @($Review.warnings)) {
            [void]$lines.Add("- $(ConvertTo-MarkdownCell $warning)")
        }
    }
    [void]$lines.Add("")
    [void]$lines.Add('A source-ledger `failed` result and an image-integrity warning are distinct: the first reports the capture runner''s outcome; the second reports what this review could read from the preserved file.')
    Write-Utf8NoBom -Path $Path -Text (($lines -join [Environment]::NewLine) + [Environment]::NewLine)
}

function Write-HtmlIndex {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][object]$Review,
        [Parameter(Mandatory = $true)][object[]]$Groups
    )

    $lines = New-Object System.Collections.Generic.List[string]
    [void]$lines.Add("<!doctype html>")
    [void]$lines.Add("<html lang=`"en`">")
    [void]$lines.Add("<head><meta charset=`"utf-8`"><meta name=`"viewport`" content=`"width=device-width, initial-scale=1`">")
    [void]$lines.Add("<title>Launcher visual-evidence review</title>")
    [void]$lines.Add("<style>body{margin:0;background:#0a0f1b;color:#f4f7ff;font-family:Segoe UI,Arial,sans-serif}main{max-width:1600px;margin:0 auto;padding:32px}h1{margin:0 0 8px;font-size:30px}h2{margin:36px 0 12px;font-size:22px}p,.muted{color:#b2bedc;line-height:1.5}.summary{display:flex;flex-wrap:wrap;gap:12px;margin:22px 0}.metric{background:#182032;border:1px solid #334563;border-radius:8px;padding:12px 14px;min-width:170px}.metric strong{display:block;font-size:20px;color:#f4f7ff}.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(360px,1fr));gap:18px}.card{display:block;background:#182032;border:1px solid #334563;border-radius:10px;padding:12px;color:inherit;text-decoration:none}.card:hover{border-color:#a98cff}.card img{display:block;width:100%;height:auto;border-radius:5px;background:#101827}.card strong{display:block;margin-top:10px}.card span{display:block;color:#b2bedc;font-size:14px;margin-top:4px}.notice{border-left:4px solid #a98cff;background:#171326;padding:14px 16px;margin:20px 0;color:#ddd7ff}</style></head>")
    [void]$lines.Add("<body><main>")
    [void]$lines.Add("<h1>Launcher visual-evidence review</h1>")
    [void]$lines.Add("<p class=`"muted`">Generated $(ConvertTo-Html $Review.generatedUtc). This gallery is a non-destructive presentation of the source capture ledger. Open a contact sheet at full size, then inspect original PNGs from the Markdown ledger for text-level decisions.</p>")
    [void]$lines.Add("<div class=`"notice`">Ledger result: $(ConvertTo-Html $Review.ledgerPassed) passed / $(ConvertTo-Html $Review.ledgerFailed) failed / $(ConvertTo-Html $Review.ledgerBlocked) blocked. Image integrity: $(ConvertTo-Html $Review.validImages) valid, $(ConvertTo-Html $Review.hashMismatchImages) hash mismatch, $(ConvertTo-Html $Review.dimensionMismatchImages) dimension mismatch, $(ConvertTo-Html $Review.invalidImages) invalid, $(ConvertTo-Html $Review.missingImages) missing.</div>")
    [void]$lines.Add("<div class=`"summary`"><div class=`"metric`"><strong>$(ConvertTo-Html $Review.totalCaptures)</strong>ledger captures</div><div class=`"metric`"><strong>$(ConvertTo-Html $Review.groupCount)</strong>surface batches</div><div class=`"metric`"><strong>$(ConvertTo-Html $Review.contactSheetCount)</strong>contact sheets</div><div class=`"metric`"><strong>$(ConvertTo-Html $Review.thumbnailWidth)px</strong>thumbnail width</div></div>")
    [void]$lines.Add("<div class=`"grid`">")
    foreach ($group in $Groups) {
        foreach ($sheet in @($group.sheets)) {
            $relative = Get-RelativePathForLink -BaseDirectory (Split-Path -Parent $Path) -TargetPath $sheet
            $sheetName = [System.IO.Path]::GetFileName($sheet)
            [void]$lines.Add(("<a class=`"card`" href=`"{0}`"><img src=`"{0}`" alt=`"{1}`"><strong>{1}</strong><span>{2} capture(s); {3} valid; {4} need attention</span><span>{5}</span></a>" -f `
                    (ConvertTo-Html $relative), (ConvertTo-Html $group.name), $group.count, $group.valid, $group.attention, (ConvertTo-Html $sheetName)))
        }
    }
    [void]$lines.Add("</div></main></body></html>")
    Write-Utf8NoBom -Path $Path -Text (($lines -join [Environment]::NewLine) + [Environment]::NewLine)
}

if ($PSCmdlet.ParameterSetName -eq "RunDirectory") {
    $runDirectoryFull = (Resolve-Path -LiteralPath $RunDirectory -ErrorAction Stop).Path
    if (-not (Get-Item -LiteralPath $runDirectoryFull).PSIsContainer) {
        throw "RunDirectory is not a directory: $runDirectoryFull"
    }
    $ledgerFullPath = Join-Path $runDirectoryFull "visual-capture-ledger.json"
    if (-not (Test-Path -LiteralPath $ledgerFullPath -PathType Leaf)) {
        throw "RunDirectory does not contain visual-capture-ledger.json: $runDirectoryFull"
    }
}
else {
    $ledgerFullPath = (Resolve-Path -LiteralPath $LedgerPath -ErrorAction Stop).Path
    $runDirectoryFull = Split-Path -Parent $ledgerFullPath
}

try {
    $ledger = Get-Content -LiteralPath $ledgerFullPath -Raw | ConvertFrom-Json
}
catch {
    throw "Could not parse capture ledger '$ledgerFullPath': $($_.Exception.Message)"
}
if ($null -eq $ledger.captures -or @($ledger.captures).Count -eq 0) {
    throw "Capture ledger has no captures: $ledgerFullPath"
}

$manifestCandidate = $ManifestPath
if ([string]::IsNullOrWhiteSpace($manifestCandidate) -and $null -ne $ledger.run -and
    -not [string]::IsNullOrWhiteSpace((ConvertTo-Text $ledger.run.manifestCopy))) {
    $manifestCandidate = Join-Path $runDirectoryFull (ConvertTo-Text $ledger.run.manifestCopy)
}
if (-not [string]::IsNullOrWhiteSpace($manifestCandidate) -and (Test-Path -LiteralPath $manifestCandidate -PathType Leaf)) {
    $manifestCandidate = (Resolve-Path -LiteralPath $manifestCandidate).Path
}
$manifestInfo = Get-ManifestCaseMap -Path $manifestCandidate

$reviewDirectory = New-UniqueReviewDirectory -RunPath $runDirectoryFull -RequestedOutputRoot $OutputRoot
$contactSheetDirectory = Join-Path $reviewDirectory "contact-sheets"
$reviewRecords = New-Object System.Collections.ArrayList
foreach ($capture in @($ledger.captures)) {
    if ($null -eq $capture) { continue }
    $record = Get-ReviewRecord -Capture $capture -LedgerDirectory $runDirectoryFull `
        -ManifestCases $manifestInfo.Cases -Grouping $GroupBy
    if ($record.canRender -and -not [string]::IsNullOrWhiteSpace($record.imagePath)) {
        $record.sourceImageRelative = Get-RelativePathForLink -BaseDirectory $reviewDirectory -TargetPath $record.imagePath
    }
    [void]$reviewRecords.Add($record)
}
if ($reviewRecords.Count -eq 0) {
    throw "Capture ledger contained no usable capture records: $ledgerFullPath"
}

$groups = New-Object System.Collections.ArrayList
$groupIndex = 0
foreach ($grouping in @($reviewRecords | Group-Object group | Sort-Object Name)) {
    $groupIndex++
    $records = @($grouping.Group | Sort-Object caseId, scroll, imageFile)
    $chunkSize = $Columns * $RowsPerSheet
    $sheetCount = [int][Math]::Ceiling($records.Count / [double]$chunkSize)
    $sheets = New-Object System.Collections.ArrayList
    for ($sheetIndex = 0; $sheetIndex -lt $sheetCount; $sheetIndex++) {
        $start = $sheetIndex * $chunkSize
        $end = [Math]::Min($start + $chunkSize - 1, $records.Count - 1)
        $sheetRecords = @($records[$start..$end])
        $stem = ConvertTo-SafeFileStem -Value $grouping.Name -Fallback ("area-{0:D2}" -f $groupIndex)
        $sheetPath = Join-Path $contactSheetDirectory ("{0:D2}-{1}-part-{2:D2}.png" -f $groupIndex, $stem, ($sheetIndex + 1))
        Write-ContactSheet -Records $sheetRecords -Group $grouping.Name `
            -SheetIndex ($sheetIndex + 1) -SheetCount $sheetCount -OutputPath $sheetPath `
            -ThumbWidth $ThumbnailWidth -ColumnCount $Columns
        [void]$sheets.Add($sheetPath)
        foreach ($sheetRecord in $sheetRecords) {
            $sheetRecord.contactSheet = Get-RelativePathForLink -BaseDirectory $reviewDirectory -TargetPath $sheetPath
        }
    }
    $valid = @($records | Where-Object { $_.integrity -eq "valid" }).Count
    $groups.Add([pscustomobject][ordered]@{
            name = $grouping.Name
            count = $records.Count
            valid = $valid
            attention = $records.Count - $valid
            sheets = @($sheets)
        }) | Out-Null
}

$ledgerPassed = @($reviewRecords | Where-Object { $_.ledgerStatus -eq "passed" }).Count
$ledgerFailed = @($reviewRecords | Where-Object { $_.ledgerStatus -eq "failed" }).Count
$ledgerBlocked = @($reviewRecords | Where-Object { $_.ledgerStatus -eq "blocked" }).Count
$provenance = Get-LedgerProvenance -Ledger $ledger -LedgerDirectory $runDirectoryFull
$warnings = New-Object System.Collections.Generic.List[string]
foreach ($warning in @($manifestInfo.Warnings)) { [void]$warnings.Add($warning) }
if ($null -eq $manifestInfo.Source) {
    [void]$warnings.Add("No manifest mapping was available; grouping falls back to route prefixes.")
}
foreach ($item in @($provenance.launcherBinary, $provenance.manifestCopy,
                     $provenance.fixtureInventory, $provenance.caseSelection)) {
    if ($null -ne $item -and $item.status -notin @("matches", "not-recorded")) {
        [void]$warnings.Add("$($item.label) provenance is $($item.status): $($item.detail)")
    }
}
if ($null -ne $provenance.fixtureInventoryGate -and
    $provenance.fixtureInventoryGate.status -notin @("passed", "not-recorded")) {
    [void]$warnings.Add("Fixture-inventory gate is $($provenance.fixtureInventoryGate.status): $($provenance.fixtureInventoryGate.detail)")
}
if ($null -ne $provenance.configurationIsolation -and
    $provenance.configurationIsolation.status -notin @("passed", "not-recorded")) {
    [void]$warnings.Add("Fixture/configuration isolation provenance is $($provenance.configurationIsolation.status): $($provenance.configurationIsolation.detail)")
}

$review = [ordered]@{
    schemaVersion = 2
    generatedUtc = [DateTime]::UtcNow.ToString("o")
    sourceLedger = $ledgerFullPath
    sourceRunDirectory = $runDirectoryFull
    manifestSource = $(if ($null -eq $manifestInfo.Source) { "not available" } else { $manifestInfo.Source })
    outputDirectory = $reviewDirectory
    groupBy = $GroupBy
    thumbnailWidth = $ThumbnailWidth
    columns = $Columns
    rowsPerSheet = $RowsPerSheet
    totalCaptures = $reviewRecords.Count
    groupCount = $groups.Count
    contactSheetCount = @($groups | ForEach-Object { @($_.sheets).Count } | Measure-Object -Sum).Sum
    ledgerPassed = $ledgerPassed
    ledgerFailed = $ledgerFailed
    ledgerBlocked = $ledgerBlocked
    validImages = @($reviewRecords | Where-Object { $_.integrity -eq "valid" }).Count
    hashMismatchImages = @($reviewRecords | Where-Object { $_.integrity -eq "hash-mismatch" }).Count
    dimensionMismatchImages = @($reviewRecords | Where-Object { $_.integrity -eq "dimension-mismatch" }).Count
    invalidImages = @($reviewRecords | Where-Object { $_.integrity -eq "invalid" }).Count
    missingImages = @($reviewRecords | Where-Object { $_.integrity -eq "missing" }).Count
    provenance = $provenance
    provenanceIssues = @($warnings | Where-Object { $_ -match "provenance" }).Count
    warnings = @($warnings)
    groups = @($groups)
    captures = @($reviewRecords)
}

$jsonPath = Join-Path $reviewDirectory "visual-review-ledger.json"
$markdownPath = Join-Path $reviewDirectory "visual-review-index.md"
$htmlPath = Join-Path $reviewDirectory "visual-review-index.html"
Write-Utf8NoBom -Path $jsonPath -Text (($review | ConvertTo-Json -Depth 12) + [Environment]::NewLine)
Write-MarkdownIndex -Path $markdownPath -Review $review -Records @($reviewRecords) -Groups @($groups)
Write-HtmlIndex -Path $htmlPath -Review $review -Groups @($groups)

Write-Output "VISUAL_REVIEW_DIRECTORY=$reviewDirectory"
Write-Output "VISUAL_REVIEW_LEDGER_JSON=$jsonPath"
Write-Output "VISUAL_REVIEW_INDEX_MARKDOWN=$markdownPath"
Write-Output "VISUAL_REVIEW_INDEX_HTML=$htmlPath"
Write-Output ("VISUAL_REVIEW_COMPLETE: {0} capture(s), {1} contact sheet(s), {2} image(s) needing integrity attention." -f `
        $review.totalCaptures, $review.contactSheetCount, ($review.totalCaptures - $review.validImages))
