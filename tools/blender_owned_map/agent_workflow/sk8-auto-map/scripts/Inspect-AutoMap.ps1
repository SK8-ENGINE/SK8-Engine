[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$Blend,

    [Parameter(Mandatory)]
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'

function Find-Blender {
    $candidates = @(
        'C:\Program Files\Blender Foundation\Blender 5.1\blender.exe',
        'C:\Program Files\Blender Foundation\Blender 5.0\blender.exe'
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }
    $command = Get-Command blender.exe -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return $command.Source
    }
    throw 'Blender 5.x was not found.'
}

$blendPath = (Resolve-Path -LiteralPath $Blend).Path
$outputPath = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $outputPath | Out-Null
$inventory = Join-Path $outputPath 'scene_inventory.json'
$members = Join-Path $outputPath 'scene_inventory_members.json'
$previews = Join-Path $outputPath 'previews'
$script = Join-Path $PSScriptRoot 'inventory_scene.py'
$blender = Find-Blender

& $blender --background $blendPath --python $script -- `
    --output $inventory `
    --members-output $members `
    --preview-directory $previews
if ($LASTEXITCODE -ne 0) {
    throw "Blender inventory failed with exit code $LASTEXITCODE"
}
if (-not (Test-Path -LiteralPath $inventory -PathType Leaf)) {
    throw "Blender did not create the expected inventory: $inventory"
}
if (-not (Test-Path -LiteralPath $members -PathType Leaf)) {
    throw "Blender did not create the expected membership sidecar: $members"
}

Write-Host "Grouped agent inventory: $inventory"
Write-Host "Script-only membership sidecar: $members"
Write-Host "Agent scene previews: $previews"
Write-Host (
    'Next: read scene_inventory.json, view every PNG under previews, read ' +
    'the references, then write a version 3 map_plan.json.'
)
