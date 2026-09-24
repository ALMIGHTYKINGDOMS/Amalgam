param(
    [string]$ProjectRef = $(if ($env:SUPABASE_PROJECT_REF) { $env:SUPABASE_PROJECT_REF } else { "nnrrmvaxnoknthpwvttt" }),
    [string]$PublishableKey = $env:SUPABASE_PUBLISHABLE_KEY
)

$ErrorActionPreference = "Stop"
$expectedProjectRef = "nnrrmvaxnoknthpwvttt"

if ($ProjectRef -ne $expectedProjectRef) {
    throw "Refusing to test project '$ProjectRef'. This Amalgam verifier is locked to '$expectedProjectRef'."
}
if ($ProjectRef -notmatch '^[a-z0-9]{20}$') {
    throw "Invalid Supabase project ref."
}

$baseUrl = "https://$ProjectRef.supabase.co"
$headers = @{}
if (-not [string]::IsNullOrWhiteSpace($PublishableKey)) {
    $headers["apikey"] = $PublishableKey
    # Supabase's current Data API requires the public key in both headers for
    # direct REST/Edge Function checks.  The launcher does the same.
    $headers["Authorization"] = "Bearer $PublishableKey"
}

function Invoke-Status([string]$Uri, [string]$Method = "GET", [hashtable]$RequestHeaders = @{}) {
    try {
        $response = Invoke-WebRequest -UseBasicParsing -Uri $Uri -Method $Method -Headers $RequestHeaders -ContentType "application/json"
        return [pscustomobject]@{ Status = [int]$response.StatusCode; Body = [string]$response.Content }
    } catch {
        $webResponse = $_.Exception.Response
        if ($null -eq $webResponse) { throw }
        $body = ""
        try {
            $reader = New-Object System.IO.StreamReader($webResponse.GetResponseStream())
            $body = $reader.ReadToEnd()
            $reader.Dispose()
        } catch { }
        return [pscustomobject]@{ Status = [int]$webResponse.StatusCode; Body = $body }
    }
}

$auth = Invoke-Status "$baseUrl/auth/v1/settings" "GET" $headers
if ([string]::IsNullOrWhiteSpace($PublishableKey) -and $auth.Status -eq 401) {
    throw @"
No publishable key was supplied. Supabase answers 401 to every request without one,
so this run would prove nothing. Set SUPABASE_PUBLISHABLE_KEY, or put the project's
publishable (anon) key in the launcher's launcher.json as supabase_anon_key.
"@
}
if ($auth.Status -ne 200) {
    throw "Supabase Auth endpoint check failed with HTTP $($auth.Status)."
}

# Supabase removed the OpenAPI schema response for anon/publishable keys. Test
# representative REST resources directly instead of probing /rest/v1/.
$clientTables = @("modpack_metadata", "network_servers")
$failedTables = @()
foreach ($table in $clientTables) {
    $tableCheck = Invoke-Status "$baseUrl/rest/v1/${table}?select=*&limit=1" "GET" $headers
    if ($tableCheck.Status -ne 200) {
        $failedTables += "$table (HTTP $($tableCheck.Status))"
    }
}
if ($failedTables.Count -gt 0) {
    throw "Representative launcher table checks failed in ${ProjectRef}: $($failedTables -join ', ')."
}
Write-Output "Schema: PASS ($($clientTables.Count) representative launcher tables reachable)"

$curseforgeUrl = "$baseUrl/functions/v1/curseforge-catalog"
$curseforge = Invoke-Status $curseforgeUrl "POST" $headers
if ($curseforge.Status -eq 404) {
    throw "curseforge-catalog is not deployed in the target project."
}
if ($curseforge.Status -ne 401) {
    throw "Protected CurseForge route returned HTTP $($curseforge.Status); expected 401 without a user session."
}

Write-Output "Supabase Auth endpoint: PASS"
Write-Output "Protected CurseForge route: PASS (401 without session)"
Write-Output "Target project: $ProjectRef"
