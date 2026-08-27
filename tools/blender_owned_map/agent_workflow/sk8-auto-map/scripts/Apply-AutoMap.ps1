[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$Blend,

    [Parameter(Mandatory)]
    [string]$Plan,

    [Parameter(Mandatory)]
    [string]$OutputBlend
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
$planPath = (Resolve-Path -LiteralPath $Plan).Path
$outputPath = [System.IO.Path]::GetFullPath($OutputBlend)
if ([string]::Equals(
    $blendPath,
    $outputPath,
    [System.StringComparison]::OrdinalIgnoreCase
)) {
    throw 'OutputBlend must be different from the source Blend.'
}

$script = Join-Path $PSScriptRoot 'apply_map_plan.py'
$blender = Find-Blender

& $blender --background $blendPath --python $script -- `
    --plan $planPath `
    --output-blend $outputPath
if ($LASTEXITCODE -ne 0) {
    throw "Blender map preparation failed with exit code $LASTEXITCODE"
}
if (-not (Test-Path -LiteralPath $outputPath -PathType Leaf)) {
    throw "Blender did not create the expected prepared map: $outputPath"
}

Write-Host "Prepared Blender map: $outputPath"
