# Reports whether packaged bridge jars match the newest gradle build outputs.
$ErrorActionPreference = 'SilentlyContinue'
$dist = 'dist/amalgam-1.0.0/bridges'
$stale = @()

# Fabric jars come from java/<mod>/build/libs; forge/neoforge from sibling trees.
$map = @{
    'amalgam-fabric-1.18.2.jar'   = 'java/fabric-1.18.2/build/libs'
    'amalgam-fabric-1.19.2.jar'   = 'java/fabric-1.19.2/build/libs'
    'amalgam-fabric-1.20.1.jar'   = 'java/fabric-1.20.1/build/libs'
    'amalgam-fabric-1.21.1.jar'   = 'java/fabric-1.21.1/build/libs'
    'amalgam-fabric-1.21.4.jar'   = 'java/fabric-1.21.4/build/libs'
    'amalgam-fabric-1.21.5.jar'   = 'java/fabric-1.21.5/build/libs'
    'amalgam-fabric-1.21.6.jar'   = 'java/fabric-1.21.6/build/libs'
    'amalgam-fabric-1.21.8.jar'   = 'java/fabric-1.21.8/build/libs'
    'amalgam-fabric-1.21.11.jar'  = 'java/fabric-1.21.11/build/libs'
    'amalgam-forge-1.12.2.jar'    = 'java-forge-1.12.2/build/libs'
    'amalgam-forge-1.18.2.jar'    = 'java-forge-legacy/forge-1.18.2/build/libs'
    'amalgam-forge-1.19.2.jar'    = 'java-forge-legacy/forge-1.19.2/build/libs'
    'amalgam-forge-1.20.1.jar'    = 'java-forge/build/libs'
    'amalgam-neoforge-1.21.1.jar' = 'java-neoforge/neoforge-1.21.1/build/libs'
    'amalgam-neoforge-1.21.4.jar' = 'java-neoforge/neoforge-1.21.4/build/libs'
    'amalgam-neoforge-1.21.5.jar' = 'java-neoforge/neoforge-1.21.5/build/libs'
    'amalgam-neoforge-1.21.6.jar' = 'java-neoforge/neoforge-1.21.6/build/libs'
    'amalgam-neoforge-1.21.8.jar' = 'java-neoforge/neoforge-1.21.8/build/libs'
    'amalgam-neoforge-1.21.11.jar'= 'java-neoforge/neoforge-1.21.11/build/libs'
}

foreach ($name in $map.Keys) {
    $packaged = Join-Path $dist $name
    $built    = Join-Path $map[$name] $name
    if (-not (Test-Path $packaged)) { Write-Output "MISSING packaged: $name"; continue }
    if (-not (Test-Path $built))    { Write-Output "NO BUILD OUTPUT: $name"; $stale += $name; continue }
    $p = (Get-Item $packaged).LastWriteTimeUtc
    $b = (Get-Item $built).LastWriteTimeUtc
    if ($b -gt $p) {
        $age = ($b - $p).TotalDays
        Write-Output ("STALE {0}: packaged {1:MM-dd HH:mm} < built {2:MM-dd HH:mm} ({3:N1} days older)" -f $name, $p, $b, $age)
        $stale += $name
    } else {
        Write-Output ("ok    {0}" -f $name)
    }
}

Write-Output ""
if ($stale.Count -gt 0) {
    Write-Output ("RESULT: " + $stale.Count + " of 19 packaged bridge jars are OLDER than their gradle build outputs - repackage required")
    exit 1
} else {
    Write-Output "RESULT: all packaged bridge jars match build outputs"
}
