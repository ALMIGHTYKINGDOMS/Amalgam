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
if ($auth.Status -ne 200) {
    throw "Supabase Auth endpoint check failed with HTTP $($auth.Status)."
}

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
