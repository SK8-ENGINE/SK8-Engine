param(
    [string]$BlenderExe = "C:\Program Files\Blender Foundation\Blender 5.0\blender.exe",
    [string]$UttRoot = $env:SKATE3_UTT_ROOT
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($UttRoot)) {
    throw "Pass -UttRoot or set SKATE3_UTT_ROOT to your UTT-1.1.7 directory."
}
$workspace = Split-Path -Parent $PSScriptRoot
$prepare = Join-Path $PSScriptRoot "prepare_university.py"
$importer = Join-Path $workspace "blender\import_university.py"
$manifest = Join-Path $workspace "intermediate\university\manifest.json"
$blend = Join-Path $workspace "blender\DIST_University.blend"

python $prepare --utt-root $UttRoot
if ($LASTEXITCODE -ne 0) {
    throw "University cache preparation failed with exit code $LASTEXITCODE"
}

& $BlenderExe --background --python-exit-code 1 --factory-startup `
    --python $importer -- $manifest $blend
if ($LASTEXITCODE -ne 0) {
    throw "Blender import failed with exit code $LASTEXITCODE"
}

Write-Host "Created $blend"
