param(
    [string]$ConfigPath = (Join-Path $PSScriptRoot "..\cpp\build\launcher.json"),
    [string]$Password = ""
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($Password)) {
    $secure = Read-Host "Local Admin password" -AsSecureString
    $ptr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try {
        $Password = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr)
    } finally {
        [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr)
    }
}

if ($Password.Length -lt 8) { throw "Admin password must contain at least 8 characters." }
if (-not (Test-Path -LiteralPath $ConfigPath)) { throw "Config not found: $ConfigPath" }

$salt = New-Object byte[] 16
$rng = [Security.Cryptography.RandomNumberGenerator]::Create()
try { $rng.GetBytes($salt) } finally { $rng.Dispose() }

$kdf = [Security.Cryptography.Rfc2898DeriveBytes]::new(
    $Password, $salt, 210000, [Security.Cryptography.HashAlgorithmName]::SHA256)
try { $verifier = $kdf.GetBytes(32) } finally { $kdf.Dispose() }

$hex = {
    param([byte[]]$Bytes)
    -join ($Bytes | ForEach-Object { $_.ToString("x2") })
}

$document = Get-Content -LiteralPath $ConfigPath -Raw | ConvertFrom-Json
$document.admin_salt = & $hex $salt
$document.admin_verifier = & $hex $verifier
$document.admin_iterations = 210000
$utf8 = New-Object System.Text.UTF8Encoding($false)
[IO.File]::WriteAllText((Resolve-Path -LiteralPath $ConfigPath).Path,
    ($document | ConvertTo-Json -Depth 20 -Compress), $utf8)

Write-Output "Local Admin password verifier updated: $ConfigPath"
