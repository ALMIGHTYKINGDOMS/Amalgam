[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$LauncherPath,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$OutputDir,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$ManifestPath,

    [ValidateNotNullOrEmpty()]
    [string[]]$Viewport = @("1920x1080", "1600x900", "1366x768"),

    # Select an exact, case-insensitive subset of manifest case IDs. Multiple
    # filters are intersected, never expanded implicitly.
    [ValidateNotNullOrEmpty()]
    [string[]]$CaseId = @(),

    # Select an exact, case-insensitive subset of manifest `surface` groups.
    # This deliberately operates on manifest metadata only; it is never passed
    # through to the launcher command line.
    [ValidateNotNullOrEmpty()]
    [string[]]$CaseGroup = @(),

    # `ManifestResponsive` applies the full manifest at its primary viewport,
    # and the compact/small high-risk lists at the matching manifest viewports.
    # An unmapped, host-valid viewport remains an all-selected-cases run and is
    # explicitly recorded as such in the selection artifact.
    [ValidateSet("All", "ManifestResponsive")]
    [string]$ViewportCasePlan = "All",

    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$')]
    [string]$RunLabel = "launcher-visual-qa",

    [ValidateRange(0, 60)]
    [int]$LauncherReadyTimeoutSeconds = 10,

    [ValidateSet("ClientArea", "Exact")]
    [string]$DimensionPolicy = "ClientArea",

    [ValidateRange(0, 128)]
    [int]$MaximumClientChromeDelta = 64,

    # A normal client-area screenshot is a few pixels smaller than the window
    # requested from the launcher. A much larger loss means Windows has
    # clamped the requested viewport to the evidence host. Broad suites stop
    # that viewport after its first failed probe instead of producing hundreds
    # of mislabeled screenshots.
    [ValidateRange(1, 1024)]
    [int]$MaterialViewportClampPixels = 96,

    # A clean exited launcher can very occasionally emit a 1x1 placeholder
    # frame while Windows is changing capture surfaces. Retry only that
    # clearly invalid artifact (or an equally material clamp) in a fresh,
    # separately recorded child environment. Zero disables the recovery path.
    [ValidateRange(0, 2)]
    [int]$TransientArtifactRetryCount = 1
)

# Captures deterministic --ui-snapshot views without ever replacing existing
# evidence. Each invocation receives a new timestamp-and-random-suffixed run
# directory beneath OutputDir, plus JSON and Markdown ledgers for review.
# Every child process receives a fresh LOCALAPPDATA/APPDATA/TEMP/TMP tree below
# that run, so fixture capture cannot read or write reviewer-owned user data.

$ErrorActionPreference = "Stop"

function Write-Utf8NoBom {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Text
    )

    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Text, $encoding)
}

function ConvertTo-Viewport {
    param([Parameter(Mandatory = $true)][string]$Value)

    $match = [System.Text.RegularExpressions.Regex]::Match(
        $Value,
        '^(?<width>[1-9][0-9]{1,4})[xX](?<height>[1-9][0-9]{1,4})$'
    )
    if (-not $match.Success) {
        throw "Viewport '$Value' must use WIDTHxHEIGHT, for example 1600x900."
    }

    $width = [int]$match.Groups['width'].Value
    $height = [int]$match.Groups['height'].Value
    if ($width -lt 320 -or $height -lt 240 -or $width -gt 16384 -or $height -gt 16384) {
        throw "Viewport '$Value' is outside the supported 320x240 through 16384x16384 range."
    }

    return [pscustomobject]@{
        Label = ("{0}x{1}" -f $width, $height)
        Width = $width
        Height = $height
    }
}

function Get-PngDimensions {
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
        if ($header[12] -ne 73 -or $header[13] -ne 72 -or $header[14] -ne 68 -or $header[15] -ne 82) {
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

    # Use the framework implementation instead of Get-FileHash.  Some
    # standalone Windows PowerShell launch contexts do not auto-load the
    # Utility module even though an interactive shell does, and a capture
    # ledger must not depend on that host-specific module behavior.
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

function Get-OptionalFileFingerprint {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return [pscustomobject][ordered]@{
            exists = $false
            bytes = $null
            sha256 = $null
        }
    }

    $item = Get-Item -LiteralPath $Path -ErrorAction Stop
    return [pscustomobject][ordered]@{
        exists = $true
        bytes = [int64]$item.Length
        sha256 = Get-Sha256File -Path $item.FullName
    }
}

function Test-EqualFileFingerprint {
    param(
        [Parameter(Mandatory = $true)][object]$Before,
        [Parameter(Mandatory = $true)][object]$After
    )

    return [bool]($Before.exists -eq $After.exists -and
                  $Before.bytes -eq $After.bytes -and
                  $Before.sha256 -eq $After.sha256)
}

function Get-FileHashWithRetry {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][int]$TimeoutSeconds,
        [Parameter(Mandatory = $true)][string]$Description
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $lastError = $null
    do {
        try {
            if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
                throw "$Description is missing."
            }
            return Get-Sha256File -Path $Path
        }
        catch {
            $lastError = $_.Exception.Message
            if ([DateTime]::UtcNow -ge $deadline) { break }
            Start-Sleep -Milliseconds 250
        }
    } while ($true)

    throw "$Description was not readable within $TimeoutSeconds second(s): $lastError"
}

function Get-PngEvidenceWithRetry {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][int]$TimeoutSeconds
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $lastError = $null
    do {
        try {
            if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
                throw "Launcher did not create the requested PNG."
            }
            $file = Get-Item -LiteralPath $Path
            [int64]$bytes = $file.Length
            if ($bytes -le 0) { throw "Launcher created an empty PNG." }
            $dimensions = Get-PngDimensions -Path $Path
            $sha256 = Get-Sha256File -Path $Path
            return [pscustomobject]@{
                Bytes = $bytes
                Width = [int64]$dimensions.Width
                Height = [int64]$dimensions.Height
                Sha256 = $sha256
            }
        }
        catch {
            $lastError = $_.Exception.Message
            if ([DateTime]::UtcNow -ge $deadline) { break }
            Start-Sleep -Milliseconds 100
        }
    } while ($true)

    throw "Could not validate the PNG within $TimeoutSeconds second(s): $lastError"
}

function Test-SnapshotDimensions {
    param(
        [Parameter(Mandatory = $true)][int64]$ActualWidth,
        [Parameter(Mandatory = $true)][int64]$ActualHeight,
        [Parameter(Mandatory = $true)][int]$RequestedWidth,
        [Parameter(Mandatory = $true)][int]$RequestedHeight,
        [Parameter(Mandatory = $true)][string]$Policy,
        [Parameter(Mandatory = $true)][int]$MaximumChromeDelta
    )

    [int64]$widthDelta = [int64]$RequestedWidth - $ActualWidth
    [int64]$heightDelta = [int64]$RequestedHeight - $ActualHeight
    if ($Policy -eq "Exact") {
        if ($widthDelta -ne 0 -or $heightDelta -ne 0) {
            throw ("PNG dimensions were {0}x{1}; expected exactly {2}x{3}." -f `
                    $ActualWidth, $ActualHeight, $RequestedWidth, $RequestedHeight)
        }
    }
    elseif ($ActualWidth -gt $RequestedWidth -or $ActualHeight -gt $RequestedHeight -or
            $widthDelta -gt $MaximumChromeDelta -or $heightDelta -gt $MaximumChromeDelta) {
        throw (("PNG dimensions were {0}x{1}; expected a client-area capture no larger than {2}x{3} " +
                "and within {4}px of each requested edge.") -f `
                $ActualWidth, $ActualHeight, $RequestedWidth, $RequestedHeight, $MaximumChromeDelta)
    }

    return [pscustomobject]@{
        Policy = $Policy
        WidthDelta = $widthDelta
        HeightDelta = $heightDelta
        MaximumClientChromeDelta = $MaximumChromeDelta
    }
}

function ConvertTo-MarkdownCell {
    param([AllowNull()][object]$Value)

    if ($null -eq $Value) { return "" }
    return (([string]$Value) -replace '[\r\n]+', ' ' -replace '\|', '\\|')
}

function Get-CaseDefinitions {
    param([Parameter(Mandatory = $true)][object]$Manifest)

    if ($null -eq $Manifest.cases) {
        throw "The visual-QA manifest must contain a non-empty 'cases' array."
    }

    $rawCases = @($Manifest.cases)
    if ($rawCases.Count -eq 0) {
        throw "The visual-QA manifest must contain at least one case."
    }

    $seenIds = @{}
    $validatedCases = New-Object System.Collections.ArrayList
    foreach ($rawCase in $rawCases) {
        if ($null -eq $rawCase) { throw "The visual-QA manifest contains an empty case." }

        $id = [string]$rawCase.id
        $route = [string]$rawCase.route
        $surface = [string]$rawCase.surface
        $state = [string]$rawCase.state
        $annotation = [string]$rawCase.annotation
        $scroll = [string]$rawCase.scroll
        if ([string]::IsNullOrWhiteSpace($id) -or $id -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,95}$') {
            throw "Each case id must use only letters, digits, '.', '_' or '-' and begin with a letter or digit."
        }
        if ($seenIds.ContainsKey($id.ToLowerInvariant())) {
            throw "The visual-QA manifest repeats case id '$id'."
        }
        $seenIds[$id.ToLowerInvariant()] = $true

        if ([string]::IsNullOrWhiteSpace($route)) {
            throw "Case '$id' is missing its required route."
        }
        # A route is passed as a single launcher argument. Reject quote/control
        # characters so an invalid manifest cannot change that command line.
        if ($route -match '["\x00-\x1F]') {
            throw "Case '$id' has an unsafe route value."
        }
        if ($surface -match '[\x00-\x08\x0B\x0C\x0E-\x1F]') {
            throw "Case '$id' has an unsafe surface value."
        }
        if ($state -match '[\x00-\x08\x0B\x0C\x0E-\x1F]') {
            throw "Case '$id' has an unsafe state value."
        }
        if ($annotation -match '[\x00-\x08\x0B\x0C\x0E-\x1F]') {
            throw "Case '$id' has an unsafe annotation value."
        }
        if ($scroll -match '[\x00-\x08\x0B\x0C\x0E-\x1F]') {
            throw "Case '$id' has an unsafe scroll value."
        }
        if ([string]::IsNullOrWhiteSpace($annotation)) { $annotation = $route }
        if ([string]::IsNullOrWhiteSpace($scroll)) { $scroll = "not-specified" }
        if ([string]::IsNullOrWhiteSpace($surface)) { $surface = "Unclassified" }
        if ([string]::IsNullOrWhiteSpace($state)) { $state = $annotation }

        [void]$validatedCases.Add([pscustomobject]@{
                Id = $id
                Route = $route
                Surface = $surface
                State = $state
                Annotation = $annotation
                Scroll = $scroll
            })
    }

    return @($validatedCases)
}

function ConvertTo-DeclaredFixtureCount {
    param(
        [Parameter(Mandatory = $true)][object]$Value,
        [Parameter(Mandatory = $true)][string]$Description
    )

    # ConvertFrom-Json represents an integer as an integer on current hosts,
    # but validate its text form as well so `172.5`, `-1`, or a quoted count
    # cannot silently become an accepted registry declaration.
    $text = ([string]$Value).Trim()
    if ($text -notmatch '^(0|[1-9][0-9]*)$') {
        throw "$Description must be a non-negative integer, but was '$text'."
    }
    try {
        return [int64]$text
    }
    catch {
        throw "$Description is outside the supported integer range: '$text'."
    }
}

function Get-LauncherFixtureInventory {
    param(
        [Parameter(Mandatory = $true)][string]$Launcher,
        [Parameter(Mandatory = $true)][int]$TimeoutSeconds
    )

    # This query is intentionally a non-UI CLI call. It opens no snapshot
    # window and reads no launcher configuration; its result proves which
    # fixture tokens the exact binary about to be captured actually accepts.
    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $Launcher
    $startInfo.Arguments = "--list-ui-fixtures"
    $startInfo.WorkingDirectory = Split-Path -Parent $Launcher
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    $stdout = $null
    $stderr = $null
    try {
        if (-not $process.Start()) {
            throw "Could not start '$Launcher --list-ui-fixtures'."
        }
        # The command returns a small JSON document. Read stdout first so its
        # pipe cannot make the process wait; stderr remains available in every
        # error path below without blending diagnostics into JSON.
        $stdout = $process.StandardOutput.ReadToEnd()
        $stderr = $process.StandardError.ReadToEnd()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            try { $process.Kill() } catch {}
            throw "Launcher fixture-inventory query exceeded $TimeoutSeconds second(s)."
        }
        if ($process.ExitCode -ne 0) {
            $detail = $stderr.Trim()
            if ([string]::IsNullOrWhiteSpace($detail)) { $detail = $stdout.Trim() }
            throw "Launcher fixture-inventory query exited with code $($process.ExitCode): $detail"
        }
    }
    finally {
        if ($null -ne $process) { $process.Dispose() }
    }

    $rawJson = $stdout.Trim()
    if ([string]::IsNullOrWhiteSpace($rawJson)) {
        throw "Launcher fixture-inventory query returned no JSON."
    }
    try {
        $inventory = $rawJson | ConvertFrom-Json -ErrorAction Stop
    }
    catch {
        throw "Launcher fixture-inventory query returned invalid JSON: $($_.Exception.Message)"
    }

    $schemaProperty = $inventory.PSObject.Properties["schemaVersion"]
    $countProperty = $inventory.PSObject.Properties["fixtureCount"]
    $fixturesProperty = $inventory.PSObject.Properties["fixtures"]
    if ($null -eq $schemaProperty -or [string]$schemaProperty.Value -ne "1") {
        throw "Launcher fixture inventory has an unsupported or missing schemaVersion."
    }
    if ($null -eq $countProperty) {
        throw "Launcher fixture inventory is missing fixtureCount."
    }
    if ($null -eq $fixturesProperty) {
        throw "Launcher fixture inventory is missing fixtures."
    }

    [int64]$declaredCount = ConvertTo-DeclaredFixtureCount -Value $countProperty.Value `
        -Description "Launcher fixture inventory fixtureCount"
    $tokens = @($fixturesProperty.Value)
    $tokenSet = @{}
    $displayTokens = New-Object System.Collections.ArrayList
    foreach ($rawToken in $tokens) {
        $token = ([string]$rawToken).Trim()
        if ($token -notmatch '^[a-z0-9][a-z0-9-]{0,127}$') {
            throw "Launcher fixture inventory contains unsafe or non-canonical token '$token'."
        }
        if ($token -cne $token.ToLowerInvariant()) {
            throw "Launcher fixture inventory contains non-lowercase token '$token'."
        }
        if ($tokenSet.ContainsKey($token)) {
            throw "Launcher fixture inventory repeats token '$token'."
        }
        $tokenSet[$token] = $true
        [void]$displayTokens.Add($token)
    }
    if ([int64]$tokens.Count -ne $declaredCount -or [int64]$tokenSet.Count -ne $declaredCount) {
        throw ("Launcher fixture inventory declares {0} token(s), but returns {1} array entries / {2} unique entries." -f `
                $declaredCount, $tokens.Count, $tokenSet.Count)
    }

    return [pscustomobject][ordered]@{
        rawJson = $rawJson
        schemaVersion = [int]$schemaProperty.Value
        fixtureCount = $declaredCount
        fixtures = @($displayTokens | Sort-Object)
        fixtureSet = $tokenSet
    }
}

function Get-ManifestFixtureRouteInventory {
    param(
        [Parameter(Mandatory = $true)][object]$Manifest,
        [Parameter(Mandatory = $true)][object[]]$Cases
    )

    $countProperty = $Manifest.PSObject.Properties["registryFixtureTokenCount"]
    if ($null -eq $countProperty) {
        throw "Visual-QA manifest is missing its required registryFixtureTokenCount declaration."
    }
    [int64]$declaredCount = ConvertTo-DeclaredFixtureCount -Value $countProperty.Value `
        -Description "Manifest registryFixtureTokenCount"

    $tokenSet = @{}
    foreach ($case in @($Cases)) {
        $route = ([string]$case.Route).Trim().ToLowerInvariant()
        $match = [System.Text.RegularExpressions.Regex]::Match(
            $route, '^(?<token>[a-z0-9][a-z0-9-]{0,127})(?:@(?<scroll>top|middle|bottom))?$')
        if (-not $match.Success) {
            throw "Case '$($case.Id)' has a route '$($case.Route)' that cannot name a launcher fixture token."
        }
        $routeScroll = $match.Groups['scroll'].Value
        $declaredScroll = ([string]$case.Scroll).Trim().ToLowerInvariant()
        if (-not [string]::IsNullOrWhiteSpace($routeScroll) -and
            $declaredScroll -in @('top', 'middle', 'bottom') -and
            $routeScroll -ne $declaredScroll) {
            throw "Case '$($case.Id)' declares scroll '$declaredScroll' but route '$($case.Route)' ends with '@$routeScroll'."
        }
        $tokenSet[$match.Groups['token'].Value] = $true
    }

    if ([int64]$tokenSet.Count -ne $declaredCount) {
        throw ("Visual-QA manifest declares {0} registry fixture token(s), but its cases resolve to {1} unique fixture route(s)." -f `
                $declaredCount, $tokenSet.Count)
    }

    return [pscustomobject][ordered]@{
        declaredFixtureCount = $declaredCount
        uniqueRouteCount = [int64]$tokenSet.Count
        fixtures = @($tokenSet.Keys | Sort-Object)
        fixtureSet = $tokenSet
    }
}

function Test-FixtureInventoryIntegrity {
    param(
        [Parameter(Mandatory = $true)][object]$LauncherInventory,
        [Parameter(Mandatory = $true)][object]$ManifestInventory
    )

    $missingFromManifest = New-Object System.Collections.ArrayList
    foreach ($token in @($LauncherInventory.fixtures)) {
        if (-not $ManifestInventory.fixtureSet.ContainsKey($token)) {
            [void]$missingFromManifest.Add($token)
        }
    }
    $unexpectedInManifest = New-Object System.Collections.ArrayList
    foreach ($token in @($ManifestInventory.fixtures)) {
        if (-not $LauncherInventory.fixtureSet.ContainsKey($token)) {
            [void]$unexpectedInManifest.Add($token)
        }
    }

    if ($LauncherInventory.fixtureCount -ne $ManifestInventory.declaredFixtureCount -or
        $LauncherInventory.fixtureCount -ne $ManifestInventory.uniqueRouteCount -or
        $missingFromManifest.Count -gt 0 -or $unexpectedInManifest.Count -gt 0) {
        $details = New-Object System.Collections.Generic.List[string]
        [void]$details.Add(("binary={0}; manifest-declared={1}; manifest-routes={2}" -f `
                $LauncherInventory.fixtureCount, $ManifestInventory.declaredFixtureCount,
                $ManifestInventory.uniqueRouteCount))
        if ($missingFromManifest.Count -gt 0) {
            [void]$details.Add("binary-only: " + (@($missingFromManifest) -join ", "))
        }
        if ($unexpectedInManifest.Count -gt 0) {
            [void]$details.Add("manifest-only: " + (@($unexpectedInManifest) -join ", "))
        }
        throw "Fixture inventory mismatch; no screenshots were started. $($details -join '; ')"
    }

    return [pscustomobject][ordered]@{
        status = "passed"
        binaryFixtureCount = [int64]$LauncherInventory.fixtureCount
        manifestDeclaredFixtureCount = [int64]$ManifestInventory.declaredFixtureCount
        manifestUniqueRouteCount = [int64]$ManifestInventory.uniqueRouteCount
        missingFromManifest = @($missingFromManifest)
        unexpectedInManifest = @($unexpectedInManifest)
    }
}

function ConvertTo-FilterSet {
    param(
        [AllowNull()][string[]]$Values,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $keys = @{}
    $normalized = New-Object System.Collections.ArrayList
    foreach ($rawValue in @($Values)) {
        $value = ([string]$rawValue).Trim()
        if ([string]::IsNullOrWhiteSpace($value)) {
            throw "$Name cannot contain an empty value."
        }
        $key = $value.ToLowerInvariant()
        if (-not $keys.ContainsKey($key)) {
            $keys[$key] = $value
            [void]$normalized.Add($value)
        }
    }

    return [pscustomobject]@{
        Keys = $keys
        Values = @($normalized)
    }
}

function Get-FilteredCases {
    param(
        [Parameter(Mandatory = $true)][object[]]$Cases,
        [AllowNull()][string[]]$RequestedCaseIds,
        [AllowNull()][string[]]$RequestedCaseGroups
    )

    $idFilter = ConvertTo-FilterSet -Values $RequestedCaseIds -Name "CaseId"
    $groupFilter = ConvertTo-FilterSet -Values $RequestedCaseGroups -Name "CaseGroup"
    $knownIds = @{}
    $knownGroups = @{}
    foreach ($case in @($Cases)) {
        $knownIds[$case.Id.ToLowerInvariant()] = $case.Id
        $knownGroups[$case.Surface.ToLowerInvariant()] = $case.Surface
    }
    foreach ($idKey in @($idFilter.Keys.Keys)) {
        if (-not $knownIds.ContainsKey($idKey)) {
            throw "CaseId filter names no manifest case: '$($idFilter.Keys[$idKey])'."
        }
    }
    foreach ($groupKey in @($groupFilter.Keys.Keys)) {
        if (-not $knownGroups.ContainsKey($groupKey)) {
            throw "CaseGroup filter names no manifest surface: '$($groupFilter.Keys[$groupKey])'."
        }
    }

    $selected = New-Object System.Collections.ArrayList
    foreach ($case in @($Cases)) {
        $matchesId = $idFilter.Keys.Count -eq 0 -or
            $idFilter.Keys.ContainsKey($case.Id.ToLowerInvariant())
        $matchesGroup = $groupFilter.Keys.Count -eq 0 -or
            $groupFilter.Keys.ContainsKey($case.Surface.ToLowerInvariant())
        if ($matchesId -and $matchesGroup) {
            [void]$selected.Add($case)
        }
    }
    if ($selected.Count -eq 0) {
        throw "The CaseId and CaseGroup filters have no cases in common."
    }

    return [pscustomobject]@{
        Cases = @($selected)
        CaseIdFilter = @($idFilter.Values)
        CaseGroupFilter = @($groupFilter.Values)
    }
}

function Get-ValidatedManifestCaseIdSet {
    param(
        [Parameter(Mandatory = $true)][object]$Manifest,
        [Parameter(Mandatory = $true)][string]$PropertyName,
        [Parameter(Mandatory = $true)][object[]]$AllCases
    )

    $property = $Manifest.PSObject.Properties[$PropertyName]
    if ($null -eq $property) {
        throw "ManifestResponsive selection requires the '$PropertyName' manifest list."
    }
    $known = @{}
    foreach ($case in @($AllCases)) { $known[$case.Id.ToLowerInvariant()] = $case.Id }

    $ids = @{}
    $display = New-Object System.Collections.ArrayList
    foreach ($rawValue in @($property.Value)) {
        $value = ([string]$rawValue).Trim()
        if ([string]::IsNullOrWhiteSpace($value)) {
            throw "Manifest list '$PropertyName' contains an empty case id."
        }
        $key = $value.ToLowerInvariant()
        if ($ids.ContainsKey($key)) {
            throw "Manifest list '$PropertyName' repeats case id '$value'."
        }
        if (-not $known.ContainsKey($key)) {
            throw "Manifest list '$PropertyName' refers to unknown case id '$value'."
        }
        $ids[$key] = $known[$key]
        [void]$display.Add($known[$key])
    }
    if ($ids.Count -eq 0) {
        throw "Manifest list '$PropertyName' must contain at least one case id."
    }
    return [pscustomobject]@{ Keys = $ids; Values = @($display) }
}

function Get-ManifestResponsiveSelectors {
    param(
        [Parameter(Mandatory = $true)][object]$Manifest,
        [Parameter(Mandatory = $true)][object[]]$AllCases
    )

    if ($null -eq $Manifest.responsiveMatrix) {
        throw "ManifestResponsive selection requires a responsiveMatrix object in the manifest."
    }

    $selectorDefinitions = @(
        [pscustomobject]@{ Key = "primaryDesktop"; CaseListProperty = $null },
        [pscustomobject]@{ Key = "compactDesktop"; CaseListProperty = "compactHighRiskCaseIds" },
        [pscustomobject]@{ Key = "smallDesktop"; CaseListProperty = "smallHighRiskCaseIds" }
    )
    $selectors = @{}
    foreach ($definition in $selectorDefinitions) {
        $sectionProperty = $Manifest.responsiveMatrix.PSObject.Properties[$definition.Key]
        if ($null -eq $sectionProperty -or $null -eq $sectionProperty.Value) {
            throw "ManifestResponsive selection requires responsiveMatrix.$($definition.Key)."
        }
        $section = $sectionProperty.Value
        $rawViewport = ([string]$section.viewport).Trim()
        if ([string]::IsNullOrWhiteSpace($rawViewport)) {
            throw "ManifestResponsive selection requires responsiveMatrix.$($definition.Key).viewport."
        }
        $viewport = ConvertTo-Viewport -Value $rawViewport
        if ($selectors.ContainsKey($viewport.Label)) {
            throw "responsiveMatrix assigns duplicate viewport '$($viewport.Label)'."
        }

        $caseIdSet = $null
        if (-not [string]::IsNullOrWhiteSpace($definition.CaseListProperty)) {
            $caseIdSet = Get-ValidatedManifestCaseIdSet -Manifest $Manifest `
                -PropertyName $definition.CaseListProperty -AllCases $AllCases
        }
        $selectors[$viewport.Label] = [pscustomobject]@{
            MatrixKey = $definition.Key
            CaseListProperty = $definition.CaseListProperty
            CaseIdSet = $caseIdSet
        }
    }
    return $selectors
}

function Get-CapturePlan {
    param(
        [Parameter(Mandatory = $true)][object]$Manifest,
        [Parameter(Mandatory = $true)][object[]]$AllCases,
        [Parameter(Mandatory = $true)][object[]]$ViewportDefinitions,
        [Parameter(Mandatory = $true)][string]$PlanMode,
        [AllowNull()][string[]]$RequestedCaseIds,
        [AllowNull()][string[]]$RequestedCaseGroups
    )

    $filterResult = Get-FilteredCases -Cases $AllCases -RequestedCaseIds $RequestedCaseIds `
        -RequestedCaseGroups $RequestedCaseGroups
    $responsiveSelectors = @{}
    if ($PlanMode -eq "ManifestResponsive") {
        $responsiveSelectors = Get-ManifestResponsiveSelectors -Manifest $Manifest -AllCases $AllCases
    }

    $viewportPlans = New-Object System.Collections.ArrayList
    foreach ($viewportDefinition in @($ViewportDefinitions)) {
        $selected = @($filterResult.Cases)
        $selectionSource = "all-selected-cases"
        $matrixKey = $null
        $caseListProperty = $null
        if ($PlanMode -eq "ManifestResponsive") {
            if ($responsiveSelectors.ContainsKey($viewportDefinition.Label)) {
                $selector = $responsiveSelectors[$viewportDefinition.Label]
                $matrixKey = $selector.MatrixKey
                $caseListProperty = $selector.CaseListProperty
                if ($null -ne $selector.CaseIdSet) {
                    $selected = @($filterResult.Cases | Where-Object {
                            $selector.CaseIdSet.Keys.ContainsKey($_.Id.ToLowerInvariant())
                        })
                    $selectionSource = "manifest-high-risk-list"
                }
                else {
                    $selectionSource = "manifest-primary-all-selected-cases"
                }
            }
            else {
                # Evidence hosts may use a safe host-maximum viewport that is
                # intentionally absent from the portability matrix. Do not
                # silently claim it is a compact/high-risk tier.
                $selectionSource = "manifest-unmapped-all-selected-cases"
            }
        }
        if ($selected.Count -eq 0) {
            $detail = if ($null -eq $caseListProperty) { $selectionSource } else { $caseListProperty }
            throw "No cases remain for viewport '$($viewportDefinition.Label)' after applying $detail and the requested filters."
        }
        [void]$viewportPlans.Add([pscustomobject]@{
                Viewport = $viewportDefinition
                Cases = @($selected)
                SelectionSource = $selectionSource
                MatrixKey = $matrixKey
                CaseListProperty = $caseListProperty
            })
    }

    return [pscustomobject]@{
        FilteredCases = @($filterResult.Cases)
        CaseIdFilter = @($filterResult.CaseIdFilter)
        CaseGroupFilter = @($filterResult.CaseGroupFilter)
        Viewports = @($viewportPlans)
    }
}

function New-UniqueRunDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Label
    )

    if (-not (Test-Path -LiteralPath $Root)) {
        [void][System.IO.Directory]::CreateDirectory($Root)
    }
    elseif (-not (Get-Item -LiteralPath $Root).PSIsContainer) {
        throw "OutputDir exists but is not a directory: $Root"
    }

    $stamp = [DateTime]::UtcNow.ToString("yyyyMMddTHHmmssfffZ")
    $suffix = [Guid]::NewGuid().ToString("N").Substring(0, 8)
    $runDirectory = Join-Path $Root ("{0}-{1}-{2}" -f $Label, $stamp, $suffix)
    if (Test-Path -LiteralPath $runDirectory) {
        throw "Refusing to reuse an existing capture directory: $runDirectory"
    }
    [void][System.IO.Directory]::CreateDirectory($runDirectory)
    return $runDirectory
}

function Get-CaptureRouteArgument {
    param([Parameter(Mandatory = $true)][object]$Case)

    # The launcher keeps scroll position inside the opaque fixture token so it
    # remains one safely quoted CLI argument. Other annotations stay ledger
    # metadata only and never affect launcher execution.
    $routeArgument = [string]$Case.Route
    if ($Case.Scroll -in @("top", "middle", "bottom")) {
        # Older manifests encode the named scroll position in both Route and
        # Scroll. Treat that as one opaque fixture token, never
        # `route@middle@middle`; reject only a contradictory declaration.
        $routeScroll = [regex]::Match($routeArgument, '@(top|middle|bottom)$')
        if ($routeScroll.Success) {
            if ($routeScroll.Groups[1].Value -ne [string]$Case.Scroll) {
                throw "Capture case '$($Case.Id)' has contradictory route and scroll values."
            }
        }
        else {
            $routeArgument = "{0}@{1}" -f $routeArgument, $Case.Scroll
        }
    }
    return $routeArgument
}

function Get-CaptureCommandDefinition {
    param(
        [Parameter(Mandatory = $true)][object]$Case,
        [Parameter(Mandatory = $true)][object]$ViewportDefinition,
        [Parameter(Mandatory = $true)][string]$ImagePath,
        [Parameter(Mandatory = $true)][string]$Launcher,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory
    )

    $routeArgument = Get-CaptureRouteArgument -Case $Case
    $argumentLine = ('--ui-snapshot "{0}" {1} {2} "{3}"' -f `
            $ImagePath, $ViewportDefinition.Width, $ViewportDefinition.Height, $routeArgument)
    return [pscustomobject][ordered]@{
        routeArgument = $routeArgument
        executable = $Launcher
        workingDirectory = $WorkingDirectory
        arguments = $argumentLine
    }
}

function New-FixtureIsolationContext {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$RelativeRoot
    )

    $rootFullPath = [System.IO.Path]::GetFullPath($Root)
    if (Test-Path -LiteralPath $rootFullPath) {
        throw "Refusing to reuse an existing child-isolation directory: $rootFullPath"
    }
    [void][System.IO.Directory]::CreateDirectory($rootFullPath)

    $values = [ordered]@{
        LOCALAPPDATA = Join-Path $rootFullPath "localappdata"
        APPDATA = Join-Path $rootFullPath "appdata"
        TEMP = Join-Path $rootFullPath "temp"
        TMP = Join-Path $rootFullPath "tmp"
    }
    foreach ($value in @($values.Values)) {
        [void][System.IO.Directory]::CreateDirectory($value)
    }
    return [pscustomobject]@{
        Root = $rootFullPath
        RelativeRoot = $RelativeRoot
        Values = $values
    }
}

function Start-IsolatedSnapshotProcess {
    param(
        [Parameter(Mandatory = $true)][object]$Command,
        [Parameter(Mandatory = $true)][string]$StdOutPath,
        [Parameter(Mandatory = $true)][string]$StdErrPath,
        [Parameter(Mandatory = $true)][object]$IsolationContext
    )

    # Windows PowerShell 5.1 lacks Start-Process -Environment. Temporarily
    # changing only this PowerShell process's inherited variables is safe here:
    # snapshots run sequentially and values are restored before the record is
    # returned. The ledger exposes both application and restoration outcomes.
    $previous = @{}
    $outcome = [pscustomobject][ordered]@{
        mode = "fresh-child-environment-per-capture"
        root = $IsolationContext.RelativeRoot
        variables = @($IsolationContext.Values.Keys)
        applied = $false
        restored = $false
        restoreError = $null
        processStartError = $null
    }
    $process = $null
    foreach ($name in @($IsolationContext.Values.Keys)) {
        $previous[$name] = [Environment]::GetEnvironmentVariable($name, "Process")
    }

    try {
        foreach ($name in @($IsolationContext.Values.Keys)) {
            [Environment]::SetEnvironmentVariable($name, [string]$IsolationContext.Values[$name], "Process")
        }
        $outcome.applied = $true
        # Start-Process receives exactly one ArgumentList string. In particular,
        # the output path stays quoted on a workspace such as "Default Project".
        $process = Start-Process -FilePath $Command.executable `
            -ArgumentList $Command.arguments `
            -WorkingDirectory $Command.workingDirectory `
            -Wait `
            -PassThru `
            -WindowStyle Hidden `
            -RedirectStandardOutput $StdOutPath `
            -RedirectStandardError $StdErrPath
    }
    catch {
        $outcome.processStartError = $_.Exception.Message
    }
    finally {
        $restoreProblems = New-Object System.Collections.Generic.List[string]
        foreach ($name in @($IsolationContext.Values.Keys)) {
            try {
                [Environment]::SetEnvironmentVariable($name, $previous[$name], "Process")
                $restoredValue = [Environment]::GetEnvironmentVariable($name, "Process")
                if ($restoredValue -ne $previous[$name]) {
                    [void]$restoreProblems.Add("$name did not return to its prior process value")
                }
            }
            catch {
                [void]$restoreProblems.Add("${name}: $($_.Exception.Message)")
            }
        }
        if ($restoreProblems.Count -eq 0) {
            $outcome.restored = $true
        }
        else {
            $outcome.restoreError = $restoreProblems -join "; "
        }
    }

    return [pscustomobject]@{
        Process = $process
        Isolation = $outcome
    }
}

function Test-MaterialViewportClamp {
    param(
        [Parameter(Mandatory = $true)][int64]$ActualWidth,
        [Parameter(Mandatory = $true)][int64]$ActualHeight,
        [Parameter(Mandatory = $true)][int]$RequestedWidth,
        [Parameter(Mandatory = $true)][int]$RequestedHeight,
        [Parameter(Mandatory = $true)][int]$ThresholdPixels
    )

    return [bool](($RequestedWidth - $ActualWidth) -gt $ThresholdPixels -or
                  ($RequestedHeight - $ActualHeight) -gt $ThresholdPixels)
}

function Get-TransientCaptureArtifactReason {
    param([Parameter(Mandatory = $true)][object]$Record)

    # A retry is deliberately narrower than general capture recovery. Process
    # crashes, missing files, arbitrary dimension failures, and launcher
    # errors retain their original failed result without a second attempt.
    if ($null -eq $Record.viewport -or $null -eq $Record.exitCode -or
        [int]$Record.exitCode -ne 0 -or $null -eq $Record.bytes -or
        [int64]$Record.bytes -le 0 -or [string]::IsNullOrWhiteSpace([string]$Record.sha256)) {
        return $null
    }

    $viewport = $Record.viewport
    if ($null -eq $viewport.actualWidth -or $null -eq $viewport.actualHeight) {
        return $null
    }
    $oneByOne = [int64]$viewport.actualWidth -eq 1 -and [int64]$viewport.actualHeight -eq 1
    if ($oneByOne -and [string]$Record.status -eq "failed" -and
        [string]$viewport.dimensionValidation -eq "failed") {
        return "PNG IHDR was exactly 1x1 after a successful launcher exit."
    }
    # This is intentionally allowed even if a caller raised the normal
    # client-chrome tolerance enough for the first record to otherwise pass:
    # the independent material-clamp threshold remains a truthful evidence
    # guard and one fresh retry can distinguish a transient placeholder from a
    # persistent host-display limit.
    if ($viewport.materiallyClamped -eq $true) {
        return ("PNG IHDR {0}x{1} was materially clamped for requested viewport {2}x{3}." -f `
                $viewport.actualWidth, $viewport.actualHeight,
                $viewport.requestedWidth, $viewport.requestedHeight)
    }
    return $null
}

function Get-RetryArtifactPath {
    param(
        [Parameter(Mandatory = $true)][string]$OriginalPath,
        [Parameter(Mandatory = $true)][int]$RetryNumber
    )

    $directory = Split-Path -Parent $OriginalPath
    $stem = [System.IO.Path]::GetFileNameWithoutExtension($OriginalPath)
    $extension = [System.IO.Path]::GetExtension($OriginalPath)
    if ([string]::IsNullOrWhiteSpace($extension)) { $extension = "" }
    return Join-Path $directory ("{0}.retry-{1}{2}" -f $stem, $RetryNumber, $extension)
}

function ConvertTo-CaptureAttemptEvidence {
    param(
        [Parameter(Mandatory = $true)][object]$Record,
        [Parameter(Mandatory = $true)][int]$Number,
        [Parameter(Mandatory = $true)][string]$Kind,
        [AllowNull()][string]$Trigger
    )

    $isOneByOne = $null -ne $Record.viewport.actualWidth -and
        $null -ne $Record.viewport.actualHeight -and
        [int64]$Record.viewport.actualWidth -eq 1 -and
        [int64]$Record.viewport.actualHeight -eq 1
    $isMaterialClamp = $Record.viewport.materiallyClamped -eq $true
    $disposition = if ($isMaterialClamp) {
        "rejected-material-clamp"
    }
    elseif ($isOneByOne) {
        "rejected-1x1-artifact"
    }
    elseif ([string]$Record.status -eq "passed") {
        "accepted"
    }
    else {
        "failed"
    }

    # Keep the complete per-attempt evidence in the JSON ledger without
    # retaining a reference to the parent record itself (which would create a
    # circular object graph during ConvertTo-Json).
    return [pscustomobject][ordered]@{
        number = $Number
        kind = $Kind
        trigger = $Trigger
        status = $Record.status
        accepted = $disposition -eq "accepted"
        disposition = $disposition
        error = $Record.error
        exitCode = $Record.exitCode
        durationMilliseconds = $Record.durationMilliseconds
        image = [ordered]@{
            file = $Record.imageFile
            bytes = $Record.bytes
            sha256 = $Record.sha256
            ihdr = [ordered]@{
                width = $Record.viewport.actualWidth
                height = $Record.viewport.actualHeight
            }
            materiallyClamped = $Record.viewport.materiallyClamped
            dimensionValidation = $Record.viewport.dimensionValidation
            dimensionValidationError = $Record.viewport.dimensionValidationError
        }
        logs = $Record.logs
        fixture = $Record.fixture
        isolation = $Record.isolation
        command = $Record.command
    }
}

function New-CaptureRecord {
    param(
        [Parameter(Mandatory = $true)][object]$Case,
        [Parameter(Mandatory = $true)][object]$ViewportDefinition,
        [Parameter(Mandatory = $true)][string]$ImagePath,
        [Parameter(Mandatory = $true)][string]$StdOutPath,
        [Parameter(Mandatory = $true)][string]$StdErrPath,
        [Parameter(Mandatory = $true)][string]$Launcher,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][int]$EvidenceReadyTimeoutSeconds,
        [Parameter(Mandatory = $true)][string]$DimensionValidationPolicy,
        [Parameter(Mandatory = $true)][int]$MaximumChromeDelta,
        [Parameter(Mandatory = $true)][int]$MaterialClampThreshold,
        [Parameter(Mandatory = $true)][string]$IsolationRoot,
        [Parameter(Mandatory = $true)][string]$IsolationRelativeRoot,
        [Parameter(Mandatory = $true)][string]$SelectionSource,
        [AllowNull()][string]$MatrixKey,
        [AllowNull()][string]$CaseListProperty
    )

    $command = Get-CaptureCommandDefinition -Case $Case -ViewportDefinition $ViewportDefinition `
        -ImagePath $ImagePath -Launcher $Launcher -WorkingDirectory $WorkingDirectory
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    $exitCode = $null
    $bytes = $null
    $sha256 = $null
    $actualWidth = $null
    $actualHeight = $null
    $widthDelta = $null
    $heightDelta = $null
    $materialViewportClamp = $null
    $dimensionValidationStatus = "not-run"
    $dimensionValidationError = $null
    $status = "passed"
    $errorMessage = $null
    $isolation = [pscustomobject][ordered]@{
        mode = "fresh-child-environment-per-capture"
        root = $IsolationRelativeRoot
        variables = @("LOCALAPPDATA", "APPDATA", "TEMP", "TMP")
        applied = $false
        restored = $false
        restoreError = $null
        processStartError = $null
    }

    try {
        $context = New-FixtureIsolationContext -Root $IsolationRoot -RelativeRoot $IsolationRelativeRoot
        $launchResult = Start-IsolatedSnapshotProcess -Command $command -StdOutPath $StdOutPath `
            -StdErrPath $StdErrPath -IsolationContext $context
        $isolation = $launchResult.Isolation
        if (-not [string]::IsNullOrWhiteSpace($isolation.processStartError)) {
            throw "Could not start isolated launcher snapshot: $($isolation.processStartError)"
        }
        if (-not $isolation.restored) {
            throw "Could not restore the capture process environment: $($isolation.restoreError)"
        }
        if ($null -eq $launchResult.Process) {
            throw "Launcher process did not start."
        }
        $exitCode = [int]$launchResult.Process.ExitCode
        if ($exitCode -ne 0) {
            throw "Launcher exited with code $exitCode."
        }

        $evidence = Get-PngEvidenceWithRetry -Path $ImagePath -TimeoutSeconds $EvidenceReadyTimeoutSeconds
        $bytes = [int64]$evidence.Bytes
        $actualWidth = [int64]$evidence.Width
        $actualHeight = [int64]$evidence.Height
        $sha256 = [string]$evidence.Sha256
        $widthDelta = [int64]$ViewportDefinition.Width - $actualWidth
        $heightDelta = [int64]$ViewportDefinition.Height - $actualHeight
        $materialViewportClamp = Test-MaterialViewportClamp -ActualWidth $actualWidth `
            -ActualHeight $actualHeight -RequestedWidth $ViewportDefinition.Width `
            -RequestedHeight $ViewportDefinition.Height -ThresholdPixels $MaterialClampThreshold

        try {
            [void](Test-SnapshotDimensions -ActualWidth $actualWidth -ActualHeight $actualHeight `
                -RequestedWidth $ViewportDefinition.Width -RequestedHeight $ViewportDefinition.Height `
                -Policy $DimensionValidationPolicy -MaximumChromeDelta $MaximumChromeDelta)
            $dimensionValidationStatus = "passed"
        }
        catch {
            $dimensionValidationStatus = "failed"
            $dimensionValidationError = $_.Exception.Message
            throw
        }
    }
    catch {
        $status = "failed"
        $errorMessage = $_.Exception.Message
    }
    finally {
        $timer.Stop()
    }

    $stdoutFingerprint = Get-OptionalFileFingerprint -Path $StdOutPath
    $stderrFingerprint = Get-OptionalFileFingerprint -Path $StdErrPath
    $fixtureDataRelativePath = Join-Path "fixture-data" ([System.IO.Path]::GetFileNameWithoutExtension($ImagePath))
    $fixtureDataPath = Join-Path (Split-Path -Parent $ImagePath) $fixtureDataRelativePath

    return [pscustomobject][ordered]@{
        caseId = $Case.Id
        surface = $Case.Surface
        state = $Case.State
        route = $Case.Route
        annotation = $Case.Annotation
        scroll = $Case.Scroll
        selection = [ordered]@{
            source = $SelectionSource
            matrixKey = $MatrixKey
            caseListProperty = $CaseListProperty
        }
        viewport = [ordered]@{
            requestedWidth = $ViewportDefinition.Width
            requestedHeight = $ViewportDefinition.Height
            actualWidth = $actualWidth
            actualHeight = $actualHeight
            widthDelta = $widthDelta
            heightDelta = $heightDelta
            policy = $DimensionValidationPolicy
            maximumClientChromeDelta = $MaximumChromeDelta
            materialClampThresholdPixels = $MaterialClampThreshold
            materiallyClamped = $materialViewportClamp
            dimensionValidation = $dimensionValidationStatus
            dimensionValidationError = $dimensionValidationError
        }
        status = $status
        attempted = $true
        exitCode = $exitCode
        durationMilliseconds = [int64]$timer.ElapsedMilliseconds
        imageFile = Split-Path -Leaf $ImagePath
        stdoutFile = Split-Path -Leaf $StdOutPath
        stderrFile = Split-Path -Leaf $StdErrPath
        bytes = $bytes
        sha256 = $sha256
        image = [ordered]@{
            file = Split-Path -Leaf $ImagePath
            bytes = $bytes
            sha256 = $sha256
            ihdr = [ordered]@{ width = $actualWidth; height = $actualHeight }
        }
        logs = [ordered]@{
            stdout = [ordered]@{ file = Split-Path -Leaf $StdOutPath; fingerprint = $stdoutFingerprint }
            stderr = [ordered]@{ file = Split-Path -Leaf $StdErrPath; fingerprint = $stderrFingerprint }
        }
        fixture = [ordered]@{
            root = $fixtureDataRelativePath
            exists = Test-Path -LiteralPath $fixtureDataPath -PathType Container
        }
        isolation = $isolation
        command = [ordered]@{
            executable = $command.executable
            workingDirectory = $command.workingDirectory
            arguments = $command.arguments
        }
        error = $errorMessage
        skippedReason = $null
    }
}

function New-BlockedCaptureRecord {
    param(
        [Parameter(Mandatory = $true)][object]$Case,
        [Parameter(Mandatory = $true)][object]$ViewportDefinition,
        [Parameter(Mandatory = $true)][string]$ImagePath,
        [Parameter(Mandatory = $true)][string]$StdOutPath,
        [Parameter(Mandatory = $true)][string]$StdErrPath,
        [Parameter(Mandatory = $true)][string]$Launcher,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][string]$SelectionSource,
        [AllowNull()][string]$MatrixKey,
        [AllowNull()][string]$CaseListProperty,
        [Parameter(Mandatory = $true)][string]$Reason,
        [Parameter(Mandatory = $true)][string]$DimensionValidationPolicy,
        [Parameter(Mandatory = $true)][int]$MaximumChromeDelta,
        [Parameter(Mandatory = $true)][int]$MaterialClampThreshold,
        [Parameter(Mandatory = $true)][string]$IsolationRelativeRoot
    )

    $command = Get-CaptureCommandDefinition -Case $Case -ViewportDefinition $ViewportDefinition `
        -ImagePath $ImagePath -Launcher $Launcher -WorkingDirectory $WorkingDirectory
    return [pscustomobject][ordered]@{
        caseId = $Case.Id
        surface = $Case.Surface
        state = $Case.State
        route = $Case.Route
        annotation = $Case.Annotation
        scroll = $Case.Scroll
        selection = [ordered]@{
            source = $SelectionSource
            matrixKey = $MatrixKey
            caseListProperty = $CaseListProperty
        }
        viewport = [ordered]@{
            requestedWidth = $ViewportDefinition.Width
            requestedHeight = $ViewportDefinition.Height
            actualWidth = $null
            actualHeight = $null
            widthDelta = $null
            heightDelta = $null
            policy = $DimensionValidationPolicy
            maximumClientChromeDelta = $MaximumChromeDelta
            materialClampThresholdPixels = $MaterialClampThreshold
            materiallyClamped = $null
            dimensionValidation = "not-run"
            dimensionValidationError = $null
        }
        status = "blocked"
        attempted = $false
        exitCode = $null
        durationMilliseconds = [int64]0
        imageFile = Split-Path -Leaf $ImagePath
        stdoutFile = Split-Path -Leaf $StdOutPath
        stderrFile = Split-Path -Leaf $StdErrPath
        bytes = $null
        sha256 = $null
        image = [ordered]@{
            file = Split-Path -Leaf $ImagePath
            bytes = $null
            sha256 = $null
            ihdr = [ordered]@{ width = $null; height = $null }
        }
        logs = [ordered]@{
            stdout = [ordered]@{ file = Split-Path -Leaf $StdOutPath; fingerprint = Get-OptionalFileFingerprint -Path $StdOutPath }
            stderr = [ordered]@{ file = Split-Path -Leaf $StdErrPath; fingerprint = Get-OptionalFileFingerprint -Path $StdErrPath }
        }
        fixture = [ordered]@{
            root = Join-Path "fixture-data" ([System.IO.Path]::GetFileNameWithoutExtension($ImagePath))
            exists = $false
        }
        isolation = [ordered]@{
            mode = "fresh-child-environment-per-capture"
            root = $IsolationRelativeRoot
            variables = @("LOCALAPPDATA", "APPDATA", "TEMP", "TMP")
            applied = $false
            restored = $null
            restoreError = $null
            processStartError = $null
        }
        command = [ordered]@{
            executable = $command.executable
            workingDirectory = $command.workingDirectory
            arguments = $command.arguments
        }
        error = $Reason
        skippedReason = $Reason
    }
}

function Write-MarkdownLedger {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][object]$Ledger
    )

    $lines = New-Object System.Collections.Generic.List[string]
    [void]$lines.Add("# Launcher visual-capture ledger")
    [void]$lines.Add("")
    [void]$lines.Add(("- **Suite:** {0}" -f (ConvertTo-MarkdownCell $Ledger.suite)))
    [void]$lines.Add(("- **Started (UTC):** {0}" -f $Ledger.run.startedUtc))
    [void]$lines.Add(("- **Finished (UTC):** {0}" -f $Ledger.run.finishedUtc))
    [void]$lines.Add(('- **Launcher:** `{0}`' -f $Ledger.run.launcherPath))
    [void]$lines.Add(('- **Launcher binary SHA-256:** `{0}`' -f $Ledger.run.launcherSha256))
    [void]$lines.Add(('- **Manifest source SHA-256:** `{0}`' -f $Ledger.run.manifestSha256))
    [void]$lines.Add(('- **Captured manifest-copy SHA-256:** `{0}`' -f $Ledger.run.manifestCopySha256))
    [void]$lines.Add(('- **Built-binary fixture inventory:** `{0}` (SHA-256 `{1}`)' -f `
            $Ledger.run.fixtureInventory, $Ledger.run.fixtureInventorySha256))
    [void]$lines.Add(('- **Fixture inventory integrity:** {0} (binary {1}; manifest declared {2}; manifest routes {3})' -f `
            $Ledger.run.fixtureInventoryIntegrity, $Ledger.run.binaryFixtureCount,
            $Ledger.run.manifestDeclaredFixtureCount, $Ledger.run.manifestUniqueFixtureRouteCount))
    [void]$lines.Add(('- **Case-selection SHA-256:** `{0}`' -f $Ledger.run.caseSelectionSha256))
    [void]$lines.Add(("- **Viewport case plan:** {0}" -f $Ledger.run.viewportCasePlan))
    $filterText = "none"
    if (@($Ledger.run.caseIdFilter).Count -gt 0 -or @($Ledger.run.caseGroupFilter).Count -gt 0) {
        $filterBits = New-Object System.Collections.Generic.List[string]
        if (@($Ledger.run.caseIdFilter).Count -gt 0) {
            [void]$filterBits.Add("case IDs: " + (@($Ledger.run.caseIdFilter) -join ", "))
        }
        if (@($Ledger.run.caseGroupFilter).Count -gt 0) {
            [void]$filterBits.Add("surfaces: " + (@($Ledger.run.caseGroupFilter) -join ", "))
        }
        $filterText = $filterBits -join "; "
    }
    [void]$lines.Add(("- **Case/group filters:** {0}" -f (ConvertTo-MarkdownCell $filterText)))
    [void]$lines.Add(("- **Fixture isolation:** {0} (fresh isolated child environment per executed attempt; configuration unchanged: {1})" -f `
            $Ledger.run.fixtureIsolation.outcome, $Ledger.run.launcherConfigUnchanged))
    [void]$lines.Add(("- **Isolation variables:** {0}" -f (@($Ledger.run.fixtureIsolation.variables) -join ", ")))
    $configBeforeHash = "missing"
    $configAfterHash = "missing"
    if ($null -ne $Ledger.run.launcherConfigBefore -and $Ledger.run.launcherConfigBefore.exists) {
        $configBeforeHash = [string]$Ledger.run.launcherConfigBefore.sha256
    }
    if ($null -ne $Ledger.run.launcherConfigAfter -and $Ledger.run.launcherConfigAfter.exists) {
        $configAfterHash = [string]$Ledger.run.launcherConfigAfter.sha256
    }
    [void]$lines.Add(('- **Launcher configuration SHA-256 (before / after):** `{0}` / `{1}`' -f `
            $configBeforeHash, $configAfterHash))
    [void]$lines.Add(("- **Outcome:** {0} passed, {1} failed, {2} blocked, {3} attempted of {4} planned" -f `
            $Ledger.summary.passed, $Ledger.summary.failed, $Ledger.summary.blocked,
            $Ledger.summary.attempted, $Ledger.summary.requested))
    [void]$lines.Add(("- **Transient-artifact recovery:** at most {0} fresh isolated retry/retries per case; {1} child capture attempt(s), {2} retry case(s), {3} recovered" -f `
            $Ledger.run.transientArtifactRetry.maximumRetries,
            $Ledger.summary.processAttempts,
            $Ledger.summary.transientArtifactRetryCases,
            $Ledger.summary.passedAfterTransientArtifactRetry))
    [void]$lines.Add("")
    [void]$lines.Add("| Case | Surface / visual state | Scroll | Viewport / PNG IHDR | Result | Exit | Isolation | Image | Bytes | SHA-256 |")
    [void]$lines.Add("| --- | --- | --- | --- | --- | ---: | --- | --- | ---: | --- |")

    foreach ($record in @($Ledger.captures)) {
        $actual = "{0}x{1}" -f $record.viewport.actualWidth, $record.viewport.actualHeight
        if ($null -eq $record.viewport.actualWidth -or $null -eq $record.viewport.actualHeight) {
            $actual = "missing"
        }
        $viewport = "{0}x{1} requested; PNG IHDR {2}" -f `
            $record.viewport.requestedWidth, $record.viewport.requestedHeight, $actual
        $image = "missing"
        if (Test-Path -LiteralPath (Join-Path (Split-Path -Parent $Path) $record.imageFile)) {
            $image = "[{0}]({0})" -f $record.imageFile
        }
        $result = [string]$record.status
        if ($record.status -ne "passed" -and -not [string]::IsNullOrWhiteSpace($record.error)) {
            $result = $record.status + ": " + (ConvertTo-MarkdownCell $record.error)
        }
        $attemptProperty = $record.PSObject.Properties["captureAttempts"]
        if ($null -ne $attemptProperty -and @($attemptProperty.Value).Count -gt 1) {
            $recovery = $record.transientArtifactRecovery
            $result += (" (after {0} attempts: {1}; prior artifact retained below)" -f `
                    @($attemptProperty.Value).Count, $recovery.outcome)
        }
        $exit = ""
        if ($null -ne $record.exitCode) { $exit = [string]$record.exitCode }
        $size = ""
        if ($null -ne $record.bytes) { $size = [string]$record.bytes }
        $hash = ""
        if ($null -ne $record.sha256) { $hash = [string]$record.sha256 }
        $isolation = "not-started"
        if ($null -ne $record.isolation) {
            if ($record.isolation.applied -and $record.isolation.restored) {
                $isolation = "isolated/restored"
            }
            elseif ($record.isolation.applied) {
                $isolation = "isolation restore failed"
            }
        }
        $surfaceAndState = "{0} - {1}" -f $record.surface, $record.state
        if ([string]::IsNullOrWhiteSpace([string]$record.surface)) {
            $surfaceAndState = "{0} - {1}" -f $record.route, $record.annotation
        }
        [void]$lines.Add(('| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} | `{9}` |' -f `
                (ConvertTo-MarkdownCell $record.caseId),
                (ConvertTo-MarkdownCell $surfaceAndState),
                (ConvertTo-MarkdownCell $record.scroll),
                (ConvertTo-MarkdownCell $viewport),
                $result,
                $exit,
                $isolation,
                $image,
                $size,
                $hash))
    }
    [void]$lines.Add("")
    $retryRecords = @($Ledger.captures | Where-Object {
            $attemptProperty = $_.PSObject.Properties["captureAttempts"]
            $null -ne $attemptProperty -and @($attemptProperty.Value).Count -gt 1
        })
    if ($retryRecords.Count -gt 0) {
        [void]$lines.Add("## Transient artifact retry evidence")
        [void]$lines.Add("")
        [void]$lines.Add("Every original artifact below is retained at its exact filename. A later retry is a distinct child process, image, log pair, and fixture-isolation root; it never overwrites or relabels the prior output.")
        [void]$lines.Add("")
        [void]$lines.Add("| Case | Attempt | Trigger | Disposition | PNG IHDR | Exit | Isolation root / outcome | Image / logs | stdout / stderr SHA-256 |")
        [void]$lines.Add("| --- | ---: | --- | --- | --- | ---: | --- | --- | --- |")
        foreach ($retryRecord in $retryRecords) {
            foreach ($attempt in @($retryRecord.captureAttempts)) {
                $attemptImage = "missing"
                if (-not [string]::IsNullOrWhiteSpace([string]$attempt.image.file) -and
                    (Test-Path -LiteralPath (Join-Path (Split-Path -Parent $Path) $attempt.image.file))) {
                    $attemptImage = "[{0}]({0})" -f $attempt.image.file
                }
                $attemptLogs = "missing"
                if ($null -ne $attempt.logs -and $null -ne $attempt.logs.stdout -and
                    $null -ne $attempt.logs.stderr) {
                    $attemptLogs = "{0} / {1}" -f $attempt.logs.stdout.file, $attempt.logs.stderr.file
                }
                $attemptImageAndLogs = $attemptImage + "<br>" + (ConvertTo-MarkdownCell $attemptLogs)
                $attemptIsolation = "not-started"
                if ($null -ne $attempt.isolation) {
                    if ($attempt.isolation.applied -and $attempt.isolation.restored) {
                        $attemptIsolation = "isolated/restored"
                    }
                    elseif ($attempt.isolation.applied) {
                        $attemptIsolation = "isolation restore failed"
                    }
                }
                $attemptIsolationDetail = $attemptIsolation
                if ($null -ne $attempt.isolation -and
                    -not [string]::IsNullOrWhiteSpace([string]$attempt.isolation.root)) {
                    $attemptIsolationDetail = "{0} ({1})" -f $attempt.isolation.root, $attemptIsolation
                }
                $attemptStdoutHash = "missing"
                $attemptStderrHash = "missing"
                if ($null -ne $attempt.logs -and $null -ne $attempt.logs.stdout -and
                    $null -ne $attempt.logs.stdout.fingerprint -and
                    $attempt.logs.stdout.fingerprint.exists) {
                    $attemptStdoutHash = [string]$attempt.logs.stdout.fingerprint.sha256
                }
                if ($null -ne $attempt.logs -and $null -ne $attempt.logs.stderr -and
                    $null -ne $attempt.logs.stderr.fingerprint -and
                    $attempt.logs.stderr.fingerprint.exists) {
                    $attemptStderrHash = [string]$attempt.logs.stderr.fingerprint.sha256
                }
                $attemptResult = [string]$attempt.disposition
                if (-not [string]::IsNullOrWhiteSpace([string]$attempt.error)) {
                    $attemptResult += ": " + (ConvertTo-MarkdownCell $attempt.error)
                }
                $attemptExit = ""
                if ($null -ne $attempt.exitCode) { $attemptExit = [string]$attempt.exitCode }
                $attemptTrigger = [string]$attempt.trigger
                if ([string]::IsNullOrWhiteSpace($attemptTrigger)) { $attemptTrigger = "initial capture" }
                [void]$lines.Add(("| {0} | {1} | {2} | {3} | {4}x{5} | {6} | {7} | {8} | `{9}` / `{10}` |" -f `
                        (ConvertTo-MarkdownCell $retryRecord.caseId),
                        $attempt.number,
                        (ConvertTo-MarkdownCell $attemptTrigger),
                        $attemptResult,
                        $attempt.image.ihdr.width, $attempt.image.ihdr.height,
                        $attemptExit,
                        (ConvertTo-MarkdownCell $attemptIsolationDetail),
                        $attemptImageAndLogs,
                        $attemptStdoutHash, $attemptStderrHash))
            }
        }
        [void]$lines.Add("")
    }
    [void]$lines.Add("## Executed or planned command evidence")
    [void]$lines.Add("")
    [void]$lines.Add("| Case | Viewport | Attempted | Exit | Command | stdout / stderr SHA-256 |")
    [void]$lines.Add("| --- | --- | --- | ---: | --- | --- |")
    foreach ($record in @($Ledger.captures)) {
        $exit = ""
        if ($null -ne $record.exitCode) { $exit = [string]$record.exitCode }
        $stdoutHash = "missing"
        $stderrHash = "missing"
        if ($null -ne $record.logs -and $null -ne $record.logs.stdout -and
            $null -ne $record.logs.stdout.fingerprint -and $record.logs.stdout.fingerprint.exists) {
            $stdoutHash = [string]$record.logs.stdout.fingerprint.sha256
        }
        if ($null -ne $record.logs -and $null -ne $record.logs.stderr -and
            $null -ne $record.logs.stderr.fingerprint -and $record.logs.stderr.fingerprint.exists) {
            $stderrHash = [string]$record.logs.stderr.fingerprint.sha256
        }
        $commandText = ""
        if ($null -ne $record.command) {
            $commandText = "`"{0}`" {1} (working directory: {2})" -f `
                $record.command.executable, $record.command.arguments, $record.command.workingDirectory
        }
        [void]$lines.Add(('| {0} | {1}x{2} | {3} | {4} | `{5}` | `{6}` / `{7}` |' -f `
                (ConvertTo-MarkdownCell $record.caseId),
                $record.viewport.requestedWidth, $record.viewport.requestedHeight,
                $record.attempted, $exit,
                (ConvertTo-MarkdownCell $commandText), $stdoutHash, $stderrHash))
    }
    [void]$lines.Add("")
    [void]$lines.Add("Every attempted image is independently checked for a non-zero file length, PNG signature, IHDR dimensions, and SHA-256 digest. The runner writes every child process into a newly created fixture-isolation LOCALAPPDATA/APPDATA/TEMP/TMP tree; it does not delete those artifacts after the run. The companion JSON ledger retains the same fields structurally, including fixture roots, log fingerprints, exact argument strings, and per-case errors.")

    Write-Utf8NoBom -Path $Path -Text (($lines -join [Environment]::NewLine) + [Environment]::NewLine)
}

if (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf)) {
    throw "Visual-QA manifest is missing: $ManifestPath"
}

$manifestFullPath = (Resolve-Path -LiteralPath $ManifestPath).Path
$outputRoot = [System.IO.Path]::GetFullPath($OutputDir)
try {
    $manifest = Get-Content -LiteralPath $manifestFullPath -Raw | ConvertFrom-Json
}
catch {
    throw "Could not parse visual-QA manifest '$manifestFullPath': $($_.Exception.Message)"
}
$cases = Get-CaseDefinitions -Manifest $manifest

$viewportDefinitions = New-Object System.Collections.ArrayList
$seenViewports = @{}
foreach ($rawViewport in $Viewport) {
    $viewportDefinition = ConvertTo-Viewport -Value $rawViewport
    if (-not $seenViewports.ContainsKey($viewportDefinition.Label)) {
        $seenViewports[$viewportDefinition.Label] = $true
        [void]$viewportDefinitions.Add($viewportDefinition)
    }
}
if ($viewportDefinitions.Count -eq 0) { throw "At least one viewport is required." }

$capturePlan = Get-CapturePlan -Manifest $manifest -AllCases $cases `
    -ViewportDefinitions @($viewportDefinitions) -PlanMode $ViewportCasePlan `
    -RequestedCaseIds $CaseId -RequestedCaseGroups $CaseGroup

$launcherFullPath = [System.IO.Path]::GetFullPath($LauncherPath)
$launcherSha256 = Get-FileHashWithRetry `
    -Path $launcherFullPath `
    -TimeoutSeconds $LauncherReadyTimeoutSeconds `
    -Description "Launcher executable '$launcherFullPath'"
$launcherFixtureInventory = Get-LauncherFixtureInventory `
    -Launcher $launcherFullPath `
    -TimeoutSeconds $LauncherReadyTimeoutSeconds
$manifestFixtureInventory = Get-ManifestFixtureRouteInventory -Manifest $manifest -Cases $cases
$fixtureInventoryIntegrity = Test-FixtureInventoryIntegrity `
    -LauncherInventory $launcherFixtureInventory `
    -ManifestInventory $manifestFixtureInventory
$launcherDirectory = Split-Path -Parent $launcherFullPath
$launcherConfigPath = Join-Path $launcherDirectory "launcher.json"
$launcherConfigBefore = Get-OptionalFileFingerprint -Path $launcherConfigPath

$runDirectory = New-UniqueRunDirectory -Root $outputRoot -Label $RunLabel
$manifestCopyPath = Join-Path $runDirectory "case-manifest.json"
Copy-Item -LiteralPath $manifestFullPath -Destination $manifestCopyPath -ErrorAction Stop
$manifestSha256 = Get-Sha256File -Path $manifestFullPath
$manifestCopySha256 = Get-Sha256File -Path $manifestCopyPath
if (-not $manifestSha256.Equals($manifestCopySha256, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "The copied visual-QA manifest hash does not match its source; capture was not started."
}

# Preserve the exact machine-readable answer from the binary whose hash is
# recorded below. This makes a later review able to prove the capture matrix
# was checked against the executable, not merely against source text.
$fixtureInventoryCopyPath = Join-Path $runDirectory "fixture-inventory.json"
Write-Utf8NoBom -Path $fixtureInventoryCopyPath -Text ($launcherFixtureInventory.rawJson + [Environment]::NewLine)
$fixtureInventorySha256 = Get-Sha256File -Path $fixtureInventoryCopyPath

$caseSelectionPath = Join-Path $runDirectory "case-selection.json"
$caseSelection = [ordered]@{
    schemaVersion = 1
    generatedUtc = [DateTime]::UtcNow.ToString("o")
    sourceManifest = $manifestFullPath
    sourceManifestSha256 = $manifestSha256
    fixtureInventory = [ordered]@{
        source = "launcher --list-ui-fixtures"
        file = "fixture-inventory.json"
        sha256 = $fixtureInventorySha256
        schemaVersion = $launcherFixtureInventory.schemaVersion
        binaryFixtureCount = $fixtureInventoryIntegrity.binaryFixtureCount
        manifestDeclaredFixtureCount = $fixtureInventoryIntegrity.manifestDeclaredFixtureCount
        manifestUniqueRouteCount = $fixtureInventoryIntegrity.manifestUniqueRouteCount
        integrity = $fixtureInventoryIntegrity.status
    }
    viewportCasePlan = $ViewportCasePlan
    filters = [ordered]@{
        caseIds = @($capturePlan.CaseIdFilter)
        caseGroups = @($capturePlan.CaseGroupFilter)
        filterSemantics = "CaseId and CaseGroup are exact case-insensitive filters; when both are set they are intersected."
    }
    selectedManifestCases = @($capturePlan.FilteredCases | ForEach-Object {
            [ordered]@{
                id = $_.Id
                surface = $_.Surface
                state = $_.State
                route = $_.Route
                scroll = $_.Scroll
            }
        })
    viewports = @($capturePlan.Viewports | ForEach-Object {
            [ordered]@{
                viewport = $_.Viewport.Label
                requestedWidth = $_.Viewport.Width
                requestedHeight = $_.Viewport.Height
                selectionSource = $_.SelectionSource
                matrixKey = $_.MatrixKey
                caseListProperty = $_.CaseListProperty
                selectedCaseIds = @($_.Cases | ForEach-Object { $_.Id })
                selectedCaseCount = @($_.Cases).Count
            }
        })
}
Write-Utf8NoBom -Path $caseSelectionPath -Text (($caseSelection | ConvertTo-Json -Depth 12) + [Environment]::NewLine)
$caseSelectionSha256 = Get-Sha256File -Path $caseSelectionPath

$startedUtc = [DateTime]::UtcNow.ToString("o")
$records = New-Object System.Collections.ArrayList

foreach ($viewportPlan in @($capturePlan.Viewports)) {
    $viewportDefinition = $viewportPlan.Viewport
    $remainingViewportBlocked = $false
    $viewportBlockReason = $null
    foreach ($case in @($viewportPlan.Cases)) {
        $baseName = "{0}--{1}" -f $case.Id, $viewportDefinition.Label
        $imagePath = Join-Path $runDirectory ($baseName + ".png")
        $stdoutPath = Join-Path $runDirectory ($baseName + ".stdout.txt")
        $stderrPath = Join-Path $runDirectory ($baseName + ".stderr.txt")
        $isolationRelativeRoot = Join-Path "fixture-isolation" $baseName
        $isolationRoot = Join-Path $runDirectory $isolationRelativeRoot
        if ((Test-Path -LiteralPath $imagePath) -or
            (Test-Path -LiteralPath $stdoutPath) -or
            (Test-Path -LiteralPath $stderrPath)) {
            throw "Refusing to overwrite an existing capture artifact: $baseName"
        }

        if ($remainingViewportBlocked) {
            $record = New-BlockedCaptureRecord `
                -Case $case `
                -ViewportDefinition $viewportDefinition `
                -ImagePath $imagePath `
                -StdOutPath $stdoutPath `
                -StdErrPath $stderrPath `
                -Launcher $launcherFullPath `
                -WorkingDirectory $launcherDirectory `
                -SelectionSource $viewportPlan.SelectionSource `
                -MatrixKey $viewportPlan.MatrixKey `
                -CaseListProperty $viewportPlan.CaseListProperty `
                -Reason $viewportBlockReason `
                -DimensionValidationPolicy $DimensionPolicy `
                -MaximumChromeDelta $MaximumClientChromeDelta `
                -MaterialClampThreshold $MaterialViewportClampPixels `
                -IsolationRelativeRoot $isolationRelativeRoot
        }
        else {
            $record = New-CaptureRecord `
                -Case $case `
                -ViewportDefinition $viewportDefinition `
                -ImagePath $imagePath `
                -StdOutPath $stdoutPath `
                -StdErrPath $stderrPath `
                -Launcher $launcherFullPath `
                -WorkingDirectory $launcherDirectory `
                -EvidenceReadyTimeoutSeconds $LauncherReadyTimeoutSeconds `
                -DimensionValidationPolicy $DimensionPolicy `
                -MaximumChromeDelta $MaximumClientChromeDelta `
                -MaterialClampThreshold $MaterialViewportClampPixels `
                -IsolationRoot $isolationRoot `
                -IsolationRelativeRoot $isolationRelativeRoot `
                -SelectionSource $viewportPlan.SelectionSource `
                -MatrixKey $viewportPlan.MatrixKey `
                -CaseListProperty $viewportPlan.CaseListProperty

            $initialTransientTrigger = Get-TransientCaptureArtifactReason -Record $record
            $attemptEvidence = New-Object System.Collections.ArrayList
            [void]$attemptEvidence.Add((ConvertTo-CaptureAttemptEvidence -Record $record -Number 1 `
                    -Kind "initial" -Trigger $initialTransientTrigger))
            $retryTrigger = $initialTransientTrigger
            $retryNumber = 0
            while ($retryNumber -lt $TransientArtifactRetryCount -and
                   -not [string]::IsNullOrWhiteSpace($retryTrigger)) {
                $retryNumber++
                $retryImagePath = Get-RetryArtifactPath -OriginalPath $imagePath -RetryNumber $retryNumber
                $retryStdOutPath = Get-RetryArtifactPath -OriginalPath $stdoutPath -RetryNumber $retryNumber
                $retryStdErrPath = Get-RetryArtifactPath -OriginalPath $stderrPath -RetryNumber $retryNumber
                $retryIsolationRelativeRoot = Join-Path $isolationRelativeRoot ("retry-{0}" -f $retryNumber)
                $retryIsolationRoot = Join-Path $runDirectory $retryIsolationRelativeRoot
                if ((Test-Path -LiteralPath $retryImagePath) -or
                    (Test-Path -LiteralPath $retryStdOutPath) -or
                    (Test-Path -LiteralPath $retryStdErrPath) -or
                    (Test-Path -LiteralPath $retryIsolationRoot)) {
                    throw "Refusing to overwrite a transient-retry capture artifact: $baseName retry $retryNumber"
                }

                $retryRecord = New-CaptureRecord `
                    -Case $case `
                    -ViewportDefinition $viewportDefinition `
                    -ImagePath $retryImagePath `
                    -StdOutPath $retryStdOutPath `
                    -StdErrPath $retryStdErrPath `
                    -Launcher $launcherFullPath `
                    -WorkingDirectory $launcherDirectory `
                    -EvidenceReadyTimeoutSeconds $LauncherReadyTimeoutSeconds `
                    -DimensionValidationPolicy $DimensionPolicy `
                    -MaximumChromeDelta $MaximumClientChromeDelta `
                    -MaterialClampThreshold $MaterialViewportClampPixels `
                    -IsolationRoot $retryIsolationRoot `
                    -IsolationRelativeRoot $retryIsolationRelativeRoot `
                    -SelectionSource $viewportPlan.SelectionSource `
                    -MatrixKey $viewportPlan.MatrixKey `
                    -CaseListProperty $viewportPlan.CaseListProperty
                [void]$attemptEvidence.Add((ConvertTo-CaptureAttemptEvidence -Record $retryRecord `
                        -Number ($retryNumber + 1) -Kind "transient-artifact-retry" -Trigger $retryTrigger))
                $record = $retryRecord
                $retryTrigger = Get-TransientCaptureArtifactReason -Record $record
            }

            $recoveryOutcome = if ($attemptEvidence.Count -eq 1) {
                "not-needed"
            }
            elseif ($record.status -eq "passed") {
                "passed-after-transient-artifact-retry"
            }
            elseif ([string]::IsNullOrWhiteSpace($retryTrigger)) {
                "retry-ended-with-nontransient-failure"
            }
            else {
                "transient-artifact-retry-exhausted"
            }
            $record | Add-Member -NotePropertyName captureAttempts -NotePropertyValue @($attemptEvidence.ToArray()) -Force
            $record | Add-Member -NotePropertyName transientArtifactRecovery -NotePropertyValue ([ordered]@{
                    policy = "fresh-isolated-retry-after-exact-1x1-or-material-clamp"
                    maximumRetries = $TransientArtifactRetryCount
                    retryCount = $retryNumber
                    attemptCount = $attemptEvidence.Count
                    initialTrigger = $initialTransientTrigger
                    outcome = $recoveryOutcome
                }) -Force

            if ($record.viewport.materiallyClamped) {
                $materialClampReason = ("Requested viewport {0} was materially clamped to PNG IHDR {1}x{2} " +
                    "(threshold {3}px). This output is not accepted as evidence; rerun on a host that can produce " +
                    "the requested viewport, or use a documented host-valid viewport.") -f `
                    $viewportDefinition.Label, $record.viewport.actualWidth, $record.viewport.actualHeight,
                    $MaterialViewportClampPixels
                if ($record.status -eq "passed") {
                    # A caller can enlarge MaximumClientChromeDelta for a
                    # special shell, but that must never turn a materially
                    # clamped artifact into a false pass.
                    $record.status = "failed"
                    $record.error = $materialClampReason
                    $record.viewport.dimensionValidation = "failed-material-clamp"
                    $record.viewport.dimensionValidationError = $materialClampReason
                }
                $recoveryProperty = $record.PSObject.Properties["transientArtifactRecovery"]
                if ($null -ne $recoveryProperty) {
                    $recoveryProperty.Value.outcome = if ($recoveryProperty.Value.retryCount -gt 0) {
                        "persistent-material-clamp-after-retry"
                    }
                    else {
                        "material-clamp-not-retried"
                    }
                }
                if (@($viewportPlan.Cases).Count -gt 1) {
                    $viewportBlockReason = $materialClampReason + " The broad $($viewportDefinition.Label) suite stopped after this probe."
                    $remainingViewportBlocked = $true
                }
            }
        }
        [void]$records.Add($record)

        $description = "{0} {1}" -f $case.Id, $viewportDefinition.Label
        $recoveryProperty = $record.PSObject.Properties["transientArtifactRecovery"]
        if ($null -ne $recoveryProperty -and $recoveryProperty.Value.retryCount -gt 0) {
            $firstAttempt = @($record.captureAttempts)[0]
            Write-Output ("CAPTURE TRANSIENT RETRY: {0}; retained {1}, final {2} ({3})" -f `
                    $description, $firstAttempt.image.file, $record.imageFile,
                    $recoveryProperty.Value.outcome)
        }
        if ($record.status -eq "passed") {
            Write-Output ("CAPTURE PASSED: {0} ({1} bytes, SHA-256 {2})" -f `
                    $description, $record.bytes, $record.sha256)
        }
        elseif ($record.status -eq "blocked") {
            Write-Warning ("CAPTURE BLOCKED: {0}: {1}" -f $description, $record.error)
        }
        else {
            Write-Warning ("CAPTURE FAILED: {0}: {1}" -f $description, $record.error)
        }
    }
}

$passed = @($records | Where-Object { $_.status -eq "passed" }).Count
$failed = @($records | Where-Object { $_.status -eq "failed" }).Count
$blocked = @($records | Where-Object { $_.status -eq "blocked" }).Count
$attempted = @($records | Where-Object { $_.attempted }).Count
$finishedUtc = [DateTime]::UtcNow.ToString("o")
$launcherConfigAfter = Get-OptionalFileFingerprint -Path $launcherConfigPath
$launcherConfigUnchanged = Test-EqualFileFingerprint -Before $launcherConfigBefore -After $launcherConfigAfter
$attemptRecords = New-Object System.Collections.ArrayList
foreach ($record in @($records)) {
    $attemptProperty = $record.PSObject.Properties["captureAttempts"]
    if ($null -ne $attemptProperty) {
        foreach ($captureAttempt in @($attemptProperty.Value)) {
            [void]$attemptRecords.Add($captureAttempt)
        }
    }
    elseif ($record.attempted) {
        [void]$attemptRecords.Add($record)
    }
}
$processAttempts = $attemptRecords.Count
$retryRecoveryRecords = @($records | Where-Object {
        $recovery = $_.PSObject.Properties["transientArtifactRecovery"]
        $null -ne $recovery -and [int]$recovery.Value.retryCount -gt 0
    })
$passedAfterTransientRetry = @($retryRecoveryRecords | Where-Object {
        $_.transientArtifactRecovery.outcome -eq "passed-after-transient-artifact-retry"
    }).Count
$allIsolationApplied = $attemptRecords.Count -gt 0 -and
    @($attemptRecords | Where-Object { -not $_.isolation.applied }).Count -eq 0
$allIsolationRestored = $attemptRecords.Count -gt 0 -and
    @($attemptRecords | Where-Object { -not $_.isolation.restored }).Count -eq 0
$fixtureIsolationOutcome = if ($launcherConfigUnchanged -and $allIsolationApplied -and $allIsolationRestored) {
    "passed"
}
else {
    "failed"
}
$suite = [string]$manifest.suite
if ([string]::IsNullOrWhiteSpace($suite)) { $suite = "Launcher visual QA" }
$ledger = [ordered]@{
    schemaVersion = 3
    suite = $suite
    run = [ordered]@{
        startedUtc = $startedUtc
        finishedUtc = $finishedUtc
        launcherPath = $launcherFullPath
        launcherSha256 = $launcherSha256
        launcherConfigPath = $launcherConfigPath
        launcherConfigBefore = $launcherConfigBefore
        launcherConfigAfter = $launcherConfigAfter
        launcherConfigUnchanged = $launcherConfigUnchanged
        manifestSource = $manifestFullPath
        manifestCopy = "case-manifest.json"
        manifestSha256 = $manifestSha256
        manifestCopySha256 = $manifestCopySha256
        fixtureInventory = "fixture-inventory.json"
        fixtureInventorySha256 = $fixtureInventorySha256
        fixtureInventorySchemaVersion = $launcherFixtureInventory.schemaVersion
        fixtureInventoryIntegrity = $fixtureInventoryIntegrity.status
        binaryFixtureCount = $fixtureInventoryIntegrity.binaryFixtureCount
        manifestDeclaredFixtureCount = $fixtureInventoryIntegrity.manifestDeclaredFixtureCount
        manifestUniqueFixtureRouteCount = $fixtureInventoryIntegrity.manifestUniqueRouteCount
        caseSelection = "case-selection.json"
        caseSelectionSha256 = $caseSelectionSha256
        outputDirectory = $runDirectory
        viewports = @($viewportDefinitions | ForEach-Object { $_.Label })
        viewportCasePlan = $ViewportCasePlan
        caseIdFilter = @($capturePlan.CaseIdFilter)
        caseGroupFilter = @($capturePlan.CaseGroupFilter)
        effectiveViewportSelections = @($caseSelection.viewports)
        dimensionPolicy = $DimensionPolicy
        maximumClientChromeDelta = $MaximumClientChromeDelta
        materialViewportClampPixels = $MaterialViewportClampPixels
        transientArtifactRetry = [ordered]@{
            maximumRetries = $TransientArtifactRetryCount
            trigger = "exact-1x1-or-material-clamp-after-exit-0-valid-png"
            retryArtifactsUseDistinctPaths = $true
            failedArtifactsArePreserved = $true
        }
        fixtureIsolation = [ordered]@{
            outcome = $fixtureIsolationOutcome
            mode = "fresh-child-environment-per-capture"
            root = "fixture-isolation"
            variables = @("LOCALAPPDATA", "APPDATA", "TEMP", "TMP")
            fixtureRootsArePerCapture = $true
            childEnvironmentAppliedForEveryAttempt = $allIsolationApplied
            childEnvironmentRestoredForEveryAttempt = $allIsolationRestored
        }
    }
    summary = [ordered]@{
        requested = $records.Count
        attempted = $attempted
        processAttempts = $processAttempts
        passed = $passed
        failed = $failed
        blocked = $blocked
        transientArtifactRetryCases = $retryRecoveryRecords.Count
        passedAfterTransientArtifactRetry = $passedAfterTransientRetry
    }
    captures = @($records)
}

$jsonLedgerPath = Join-Path $runDirectory "visual-capture-ledger.json"
$markdownLedgerPath = Join-Path $runDirectory "visual-capture-ledger.md"
Write-Utf8NoBom -Path $jsonLedgerPath -Text (($ledger | ConvertTo-Json -Depth 12) + [Environment]::NewLine)
Write-MarkdownLedger -Path $markdownLedgerPath -Ledger $ledger

Write-Output "VISUAL_CAPTURE_RUN=$runDirectory"
Write-Output "VISUAL_CAPTURE_LEDGER_JSON=$jsonLedgerPath"
Write-Output "VISUAL_CAPTURE_LEDGER_MARKDOWN=$markdownLedgerPath"

if ($failed -gt 0 -or $blocked -gt 0 -or $fixtureIsolationOutcome -ne "passed") {
    $configurationDetail = if ($fixtureIsolationOutcome -eq "passed") { "" } else { " Fixture/configuration isolation did not pass." }
    throw "Visual capture produced $failed failed and $blocked blocked of $($records.Count) planned case(s).$configurationDetail Inspect $markdownLedgerPath and the matching stdout/stderr files."
}

Write-Output "VISUAL_CAPTURE_COMPLETE: $passed capture(s) passed."
