[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$toolRoot = $PSScriptRoot
$sourceRoot = Join-Path $toolRoot 'owned_world_material_addon'
$archivePath = Join-Path $toolRoot 'owned_world_material_addon.zip'
$requiredFiles = @(
    '__init__.py',
    'blender_manifest.toml',
    'exporter.py'
)

foreach ($name in $requiredFiles) {
    $path = Join-Path $sourceRoot $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required addon source is missing: $path"
    }
}

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

if (Test-Path -LiteralPath $archivePath -PathType Leaf) {
    Remove-Item -LiteralPath $archivePath -Force
}

$stream = [System.IO.File]::Open(
    $archivePath,
    [System.IO.FileMode]::CreateNew,
    [System.IO.FileAccess]::ReadWrite,
    [System.IO.FileShare]::None
)
try {
    $archive = [System.IO.Compression.ZipArchive]::new(
        $stream,
        [System.IO.Compression.ZipArchiveMode]::Create,
        $true
    )
    try {
        foreach ($name in $requiredFiles) {
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $archive,
                (Join-Path $sourceRoot $name),
                $name,
                [System.IO.Compression.CompressionLevel]::Optimal
            ) | Out-Null
        }
    } finally {
        $archive.Dispose()
    }
} finally {
    $stream.Dispose()
}

$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archivePath).Hash
Write-Host "Blender addon: $archivePath"
Write-Host "SHA-256:      $hash"
