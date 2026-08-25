param(
    [string]$BlenderExe = "C:\Program Files\Blender Foundation\Blender 5.0\blender.exe",
    [string]$UttRoot = $env:SKATE3_UTT_ROOT
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($UttRoot)) {
    throw "Pass -UttRoot or set SKATE3_UTT_ROOT to your UTT-1.1.7 directory."
}
if (-not (Test-Path -LiteralPath $BlenderExe -PathType Leaf)) {
    throw "Blender was not found at $BlenderExe"
}

$workspace = Split-Path -Parent $PSScriptRoot
$prepare = Join-Path $PSScriptRoot "prepare_skate2_bam.py"
$collisionBuilder = Join-Path $PSScriptRoot "build_retail_collision_archive.py"
$importer = Join-Path $workspace "blender\import_hawaiian_dream.py"
$manifest = Join-Path $workspace "intermediate\skate2_bam\manifest.json"
$collisionArchive = Join-Path (
    $workspace
) "intermediate\skate2_bam\Skate_2_New_San_Vanelona.spawn-collision.rwcmset"
$blend = Join-Path $workspace "blender\Skate_2_New_San_Vanelona.blend"

python $prepare --utt-root $UttRoot
if ($LASTEXITCODE -ne 0) {
    throw "Skate 2 BAM cache preparation failed with exit code $LASTEXITCODE"
}

python $collisionBuilder $manifest $collisionArchive `
    --expected-asset-count 753 --expected-mesh-count 792
if ($LASTEXITCODE -ne 0) {
    throw "Skate 2 retail collision archive failed with exit code $LASTEXITCODE"
}

& $BlenderExe --background --python-exit-code 1 --factory-startup `
    --python $importer -- $manifest $blend
if ($LASTEXITCODE -ne 0) {
    throw "Blender import failed with exit code $LASTEXITCODE"
}

Write-Host "Created $blend"
Write-Host "Created $collisionArchive"
