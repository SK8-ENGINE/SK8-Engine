[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]*$')]
    [string]$Version,

    [string]$BuildDirectory = 'out\build\release',

    [string]$OutputDirectory = 'out\packages',

    [string]$PackageName = 'Skate3CustomEngineLayer'
)

$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot '..')
)
$buildRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $repoRoot $BuildDirectory)
)
$outputRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $repoRoot $OutputDirectory)
)
$archiveBase = "$PackageName-$Version-Windows"
$stageRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $outputRoot $archiveBase)
)
$archivePath = [System.IO.Path]::GetFullPath(
    (Join-Path $outputRoot "$archiveBase.zip")
)
$addonBuildScript = Join-Path (
    Join-Path $repoRoot 'tools\blender_owned_map'
) 'Build-Addon.ps1'

if (-not $stageRoot.StartsWith(
        $outputRoot + [System.IO.Path]::DirectorySeparatorChar,
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Package stage escaped its output directory: $stageRoot"
}

$executable = Join-Path $buildRoot 'skate3.exe'
$runtime = Join-Path $buildRoot 'rexruntime.dll'
foreach ($required in @($executable, $runtime)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Release build file is missing: $required"
    }
}

$tuFunctionsPath = Join-Path $repoRoot 'config\skate3_tu_functions.toml'
$registerPath = Join-Path $repoRoot 'generated\skate3_register.cpp'
foreach ($required in @($tuFunctionsPath, $registerPath)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "TU3 function coverage input is missing: $required"
    }
}

$requiredTuFunctions = [System.Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase
)
foreach ($line in Get-Content -LiteralPath $tuFunctionsPath) {
    if ($line -match '^\s*"(0x[0-9A-Fa-f]{8})"\s*=') {
        [void]$requiredTuFunctions.Add($matches[1])
    }
}
$registeredFunctions = [System.Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase
)
$registerContents = Get-Content -LiteralPath $registerPath -Raw
foreach ($match in [regex]::Matches(
        $registerContents,
        'SetFunction\((0x[0-9A-Fa-f]{8}),'
    )) {
    [void]$registeredFunctions.Add($match.Groups[1].Value)
}
$missingTuFunctions = @(
    $requiredTuFunctions |
        Where-Object { -not $registeredFunctions.Contains($_) } |
        Sort-Object
)
if ($missingTuFunctions.Count -gt 0) {
    $sample = ($missingTuFunctions | Select-Object -First 20) -join ', '
    throw @"
Generated function registration is incomplete for TU3.
Missing $($missingTuFunctions.Count) of $($requiredTuFunctions.Count) roots.
First missing roots: $sample
Refusing to package a build that will terminate after installing default.xexp.
"@
}
Write-Host (
    "Verified TU3 function coverage: " +
    "$($requiredTuFunctions.Count)/$($requiredTuFunctions.Count) roots registered."
)

if (-not (Test-Path -LiteralPath $addonBuildScript -PathType Leaf)) {
    throw "Blender addon build script is missing: $addonBuildScript"
}
& $addonBuildScript

New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
if (Test-Path -LiteralPath $stageRoot) {
    Remove-Item -LiteralPath $stageRoot -Recurse -Force
}
if (Test-Path -LiteralPath $archivePath) {
    Remove-Item -LiteralPath $archivePath -Force
}

New-Item -ItemType Directory -Path $stageRoot -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $stageRoot 'maps') -Force |
    Out-Null
New-Item -ItemType Directory -Path (
    Join-Path $stageRoot 'Blender Map Tools'
) -Force | Out-Null
New-Item -ItemType Directory -Path (
    Join-Path $stageRoot 'Vanilla Map Extraction Tools'
) -Force | Out-Null

Copy-Item -LiteralPath $executable -Destination (
    Join-Path $stageRoot 'skate3.exe'
)
Copy-Item -LiteralPath $runtime -Destination (
    Join-Path $stageRoot 'rexruntime.dll'
)
Copy-Item -LiteralPath (Join-Path $repoRoot 'release\README.txt') -Destination (
    Join-Path $stageRoot 'README.txt'
)
Copy-Item -LiteralPath (Join-Path $repoRoot 'CUSTOM_MAPS.md') -Destination (
    Join-Path $stageRoot 'CUSTOM_MAPS.md'
)
Copy-Item -LiteralPath (Join-Path $repoRoot 'MULTIPLAYER.md') -Destination (
    Join-Path $stageRoot 'MULTIPLAYER.md'
)
foreach ($document in @(
    'LICENSE-PROJECT.md',
    'NOTICE.md',
    'CHANGELOG.md',
    'KNOWN_ISSUES.md'
)) {
    Copy-Item -LiteralPath (Join-Path $repoRoot $document) -Destination (
        Join-Path $stageRoot $document
    )
}
Copy-Item -LiteralPath (
    Join-Path $repoRoot 'third_party\rexglue-sdk\LICENSE'
) -Destination (
    Join-Path $stageRoot 'LICENSE-rexglue.txt'
)
Copy-Item -LiteralPath (Join-Path $repoRoot 'release\maps\README.txt') `
    -Destination (Join-Path $stageRoot 'maps\README.txt')
foreach ($bundledMapFile in @(
    'blender_bake_showcase.skate',
    'blender_bake_showcase.blend'
)) {
    Copy-Item -LiteralPath (
        Join-Path $repoRoot "maps\$bundledMapFile"
    ) -Destination (
        Join-Path $stageRoot "maps\$bundledMapFile"
    )
}
Copy-Item -LiteralPath (
    Join-Path $repoRoot 'tools\blender_owned_map\owned_world_material_addon.zip'
) -Destination (
    Join-Path $stageRoot 'Blender Map Tools\owned_world_material_addon.zip'
)
Copy-Item -LiteralPath (
    Join-Path $repoRoot 'tools\blender_owned_map\README.md'
) -Destination (
    Join-Path $stageRoot 'Blender Map Tools\README.md'
)
Copy-Item -LiteralPath (
    Join-Path $repoRoot 'tools\blender_owned_map\SKATE_FORMAT.md'
) -Destination (
    Join-Path $stageRoot 'Blender Map Tools\SKATE_FORMAT.md'
)

$extractionSource = Join-Path $repoRoot 'tools\vanilla_map_extraction'
$extractionStage = Join-Path $stageRoot 'Vanilla Map Extraction Tools'
foreach ($document in @('README.md', 'UNIVERSITY_INTEGRATION.md')) {
    Copy-Item -LiteralPath (
        Join-Path $extractionSource $document
    ) -Destination (
        Join-Path $extractionStage $document
    )
}
foreach ($directory in @('blender', 'schemas', 'tests', 'tools')) {
    $sourceDirectory = Join-Path $extractionSource $directory
    $stageDirectory = Join-Path $extractionStage $directory
    New-Item -ItemType Directory -Path $stageDirectory -Force | Out-Null
    Get-ChildItem -LiteralPath $sourceDirectory -File |
        Where-Object {
            $_.Extension.ToLowerInvariant() -in @(
                '.py', '.ps1', '.json', '.md'
            )
        } |
        ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination (
                Join-Path $stageDirectory $_.Name
            )
        }
}

$forbiddenExtensions = @(
    '.iso', '.xex', '.xexp', '.skate', '.blend', '.blend1', '.big',
    '.stfs', '.sav', '.log', '.dmp', '.png', '.jpg', '.jpeg', '.exr',
    '.dds', '.pyc'
)
$forbiddenNames = @(
    'default.xex', 'default.xexp', 'EAWebkit.xex', 'EAWebkit.xexp',
    'active_map.txt', 'settings.toml'
)
$allowedFirstPartyMapFiles = @(
    'maps/blender_bake_showcase.skate',
    'maps/blender_bake_showcase.blend'
)
$stagedFiles = Get-ChildItem -LiteralPath $stageRoot -Recurse -File
foreach ($file in $stagedFiles) {
    $relative = [System.IO.Path]::GetRelativePath(
        $stageRoot, $file.FullName
    ).Replace('\', '/')
    $isAllowedFirstPartyMap =
        $allowedFirstPartyMapFiles -contains $relative
    if ((-not $isAllowedFirstPartyMap -and
         $forbiddenExtensions -contains $file.Extension.ToLowerInvariant()) -or
        $forbiddenNames -contains $file.Name) {
        throw "Forbidden release payload detected: $($file.FullName)"
    }
}

$checksums = foreach ($file in $stagedFiles | Sort-Object FullName) {
    $relative = [System.IO.Path]::GetRelativePath(
        $stageRoot, $file.FullName
    ).Replace('\', '/')
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash
    "$hash  $relative"
}
$checksums | Set-Content -LiteralPath (
    Join-Path $stageRoot 'SHA256SUMS.txt'
) -Encoding ascii

Compress-Archive -LiteralPath $stageRoot -DestinationPath $archivePath `
    -CompressionLevel Optimal

$archiveHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archivePath).Hash
$archiveSize = (Get-Item -LiteralPath $archivePath).Length
$manifestPath = Join-Path $outputRoot 'update-manifest.toml'
@"
version = "$Version"
asset_url = "https://github.com/SK8-ENGINE/SK8-Engine/releases/download/v$Version/$archiveBase.zip"
sha256 = "$($archiveHash.ToLowerInvariant())"
size = $archiveSize
"@ | Set-Content -LiteralPath $manifestPath -Encoding ascii
Write-Host "Release package: $archivePath"
Write-Host "SHA-256:        $archiveHash"
Write-Host "Update manifest: $manifestPath"
Write-Host "Files:          $((Get-ChildItem -LiteralPath $stageRoot -Recurse -File).Count)"
