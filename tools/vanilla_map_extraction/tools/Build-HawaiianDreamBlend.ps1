param(
    [string]$BlenderExe = "C:\Program Files\Blender Foundation\Blender 5.0\blender.exe",
    [string]$UttRoot = $env:SKATE3_UTT_ROOT
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($UttRoot)) {
    throw "Pass -UttRoot or set SKATE3_UTT_ROOT to your UTT-1.1.7 directory."
}
$workspace = Split-Path -Parent $PSScriptRoot
$prepare = Join-Path $PSScriptRoot "prepare_hawaiian_dream.py"
$importer = Join-Path $workspace "blender\import_hawaiian_dream.py"
$manifest = Join-Path $workspace "intermediate\hawaiian_dream\manifest.json"
$blend = Join-Path $workspace "blender\Danny_Way_Hawaiian_Dream.blend"

python $prepare --utt-root $UttRoot
if ($LASTEXITCODE -ne 0) {
    throw "Hawaiian Dream cache preparation failed with exit code $LASTEXITCODE"
}

& $BlenderExe --background --factory-startup --python $importer -- $manifest $blend
if ($LASTEXITCODE -ne 0) {
    throw "Blender import failed with exit code $LASTEXITCODE"
}

Write-Host "Created $blend"
