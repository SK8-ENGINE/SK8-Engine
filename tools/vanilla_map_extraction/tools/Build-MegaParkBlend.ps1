param(
    [string]$BlenderExe = "C:\Program Files\Blender Foundation\Blender 5.0\blender.exe"
)

$ErrorActionPreference = "Stop"
$workspace = Split-Path -Parent $PSScriptRoot
$prepare = Join-Path $PSScriptRoot "prepare_mega_park.py"
$importer = Join-Path $workspace "blender\import_mega_park.py"
$manifest = Join-Path $workspace "intermediate\mega_park\manifest.json"
$blend = Join-Path $workspace "blender\DIST_MegaPark.blend"
$preview = Join-Path $workspace "blender\DIST_MegaPark_preview.png"

python $prepare
if ($LASTEXITCODE -ne 0) {
    throw "Mega Park cache preparation failed with exit code $LASTEXITCODE"
}

& $BlenderExe --background --factory-startup --python $importer -- $manifest $blend $preview
if ($LASTEXITCODE -ne 0) {
    throw "Blender import failed with exit code $LASTEXITCODE"
}

Write-Host "Created $blend"
Write-Host "Created $preview"
