param(
    [string]$BuildDir = 'cpp/build-release'
)

$ErrorActionPreference = 'Stop'
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$tests = @(Get-ChildItem -LiteralPath $BuildDir -Filter 'amalgam_*_test.exe' -File |
    Sort-Object Name)
if ($tests.Count -eq 0) {
    throw "No native test executables were found in $BuildDir"
}
$fail = 0
$passed = 0
foreach ($t in $tests) {
    $output = & $t.FullName 2>&1
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        Write-Output ("FAIL: " + $t.Name + " exit=" + $exitCode)
        if ($output) { $output | Write-Output }
        $fail++
    } else {
        Write-Output ("PASS: " + $t.Name)
        $passed++
    }
}
Write-Output ("TOTAL: " + $tests.Count + " PASSED: " + $passed + " FAILED: " + $fail)
if ($fail -gt 0) { exit 1 }
