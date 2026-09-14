# Fails when any packaged .jar is older than a java source file, or when the
# release package/installer are older than native binaries or key sources.
$ErrorActionPreference = 'Stop'
$fail = $false

$javaSources = Get-ChildItem -Path 'java' -Recurse -Filter '*.java' -ErrorAction SilentlyContinue
if ($javaSources) {
    $jars = Get-ChildItem -Path 'dist/amalgam-1.0.0' -Recurse -Filter '*.jar' -ErrorAction SilentlyContinue
    foreach ($j in $jars) {
        $newer = @($javaSources | Where-Object { $_.LastWriteTime -gt $j.LastWriteTime })
        if ($newer.Count -gt 0) {
            $newest = ($newer | Sort-Object LastWriteTime -Descending | Select-Object -First 1)
            Write-Output ("STALE JAR: " + $j.FullName + " (" + $j.LastWriteTime + ") older than " + $newest.FullName + " (" + $newest.LastWriteTime + ")")
            $fail = $true
        }
    }
    if (-not $fail) { Write-Output ("jars: " + $jars.Count + " packaged jar(s) newer than all java sources - OK") }
} else {
    Write-Output "jars: no java sources found - skipped"
}

$zip = 'dist/amalgam-1.0.0.zip'
$exe = 'cpp/build-release/amalgam_launcher.exe'
if ((Test-Path $zip) -and (Test-Path $exe)) {
    if ((Get-Item $exe).LastWriteTime -gt (Get-Item $zip).LastWriteTime) {
        Write-Output "STALE PACKAGE: $zip is older than $exe - repackage required"
        $fail = $true
    } else {
        Write-Output "package: $zip newer than launcher binary - OK"
    }
}

if ($fail) { exit 1 } else { Write-Output "freshness: PASS" }
