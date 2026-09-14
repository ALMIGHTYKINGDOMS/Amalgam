param(
    [Parameter(Mandatory = $true)]
    [string]$SourceDir
)

$ErrorActionPreference = 'Stop'
$SourceDir = (Resolve-Path -LiteralPath $SourceDir).Path
$required = @(
    'amalgam_launcher.exe', 'amalgam.dll', 'launcher.json.template',
    'prerequisites.json', 'component-manifest.json', 'sbom.cdx.json', 'release.sha256',
    'LICENSE.txt', 'INSTALL-INFO.txt', 'USER-GUIDE.md', 'RELEASE-NOTES.md',
    'bridges', 'bedrock\AmalgamBedrockClient.mcaddon',
    'branding', 'ai\ai-manifest.json', 'ai\ai-package-manifest.json',
    'ai\knowledge', 'legal\TERMS.txt', 'legal\PRIVACY.txt',
    'legal\AI_TERMS.txt', 'legal\THIRD_PARTY_NOTICES.txt',
    'tools\ai-bootstrap.ps1', 'tools\ai-bootstrap.cmd',
    'runtimes\ai\llama', 'runtimes\ai\sd'
)
foreach ($relative in $required) {
    $path = Join-Path $SourceDir $relative
    if (-not (Test-Path -LiteralPath $path)) { throw "Missing installer input: $relative" }
}
$jars = @(Get-ChildItem -LiteralPath (Join-Path $SourceDir 'bridges') -Filter '*.jar' -File)
if ($jars.Count -ne 19) { throw "Expected 19 bridges, found $($jars.Count)" }
Write-Output "Installer inputs passed: $SourceDir"
