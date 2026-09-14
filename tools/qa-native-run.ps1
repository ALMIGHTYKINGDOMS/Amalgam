param(
    [string]$BuildDir = 'cpp/build-release'
)

$ErrorActionPreference = 'Continue'
$tests = Get-ChildItem $BuildDir -Filter 'amalgam_*_test.exe'
$fail = 0
$passed = 0
foreach ($t in $tests) {
    & $t.FullName *> $null
    if ($LASTEXITCODE -ne 0) {
        Write-Output ("FAIL: " + $t.Name + " exit=" + $LASTEXITCODE)
        $fail++
    } else {
        Write-Output ("PASS: " + $t.Name)
        $passed++
    }
}
Write-Output ("TOTAL: " + $tests.Count + " PASSED: " + $passed + " FAILED: " + $fail)
if ($fail -gt 0) { exit 1 }
