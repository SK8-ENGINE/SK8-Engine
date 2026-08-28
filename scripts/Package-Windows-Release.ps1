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

function Get-PortableRelativePath {
    param(
        [Parameter(Mandatory)][string]$BasePath,
        [Parameter(Mandatory)][string]$TargetPath
    )

    $baseFull = [System.IO.Path]::GetFullPath($BasePath)
    if (-not $baseFull.EndsWith(
            [System.IO.Path]::DirectorySeparatorChar.ToString())) {
        $baseFull += [System.IO.Path]::DirectorySeparatorChar
    }
    $targetFull = [System.IO.Path]::GetFullPath($TargetPath)
    $baseUri = [System.Uri]::new($baseFull)
    $targetUri = [System.Uri]::new($targetFull)
    if ($baseUri.Scheme -ne $targetUri.Scheme) {
        throw "Cannot relativize paths on different schemes."
    }
    return [System.Uri]::UnescapeDataString(
        $baseUri.MakeRelativeUri($targetUri).ToString()
    ).Replace('/', [System.IO.Path]::DirectorySeparatorChar)
}

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
$defaultObjectCategories = @(
    'Branded',
    'DMO_Barriers',
    'DMO_Bins',
    'DMO_Boxes',
    'DMO_Misc',
    'DMO_Rails',
    'DMO_Ramps',
    'DMO_Tables',
    'DownTown',
    'Foliage',
    'Hubbas',
    'Mega',
    'Misc',
    'OTS',
    'Plaza',
    'PreMade',
    'Rails',
    'Street',
    'Vert'
)

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

$dlssEnabled = $false
$dlssRuntimeHashes = [ordered]@{
    'sl.interposer.dll' =
        '2a79db6857ae8c75bbd871a9489c48bc6a39f7fcc88b9b02afd53d0376cbec66'
    'sl.common.dll' =
        'c57930ef5a8a3fe9be85efdf71a61d8107c1148e8a6aed456464547128f7f4ae'
    'sl.dlss.dll' =
        'a997022d2b93601e0eefc3ddb3067c36df386dd3163ae71e11095191fb14f8e4'
    'nvngx_dlss.dll' =
        'be6e434a94ca32499515eb62ca0e6c274526055d568d0426e4c652dcdfb6ee6e'
}
$cmakeCache = Join-Path $buildRoot 'CMakeCache.txt'
if (Test-Path -LiteralPath $cmakeCache -PathType Leaf) {
    $dlssEnabled = [bool](
        Select-String -LiteralPath $cmakeCache -SimpleMatch `
            'SKATE3_ENABLE_DLSS_SR:BOOL=ON'
    )
    $dlssNrPreview = [bool](
        Select-String -LiteralPath $cmakeCache -SimpleMatch `
            'SKATE3_ENABLE_DLSS_NR_PREVIEW:BOOL=ON'
    )
    if ($dlssNrPreview) {
        throw (
            'Public packaging is disabled for the private Streamline 2.13 ' +
            'DLSS Neural Rendering preview because complete redistribution ' +
            'terms and notices were not supplied.'
        )
    }
}
if ($dlssEnabled) {
    foreach ($entry in $dlssRuntimeHashes.GetEnumerator()) {
        $path = Join-Path $buildRoot $entry.Key
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "DLSS-enabled build is missing official runtime: $path"
        }
        $actual = (
            Get-FileHash -Algorithm SHA256 -LiteralPath $path
        ).Hash.ToLowerInvariant()
        if ($actual -ne $entry.Value) {
            throw (
                "Unexpected SHA-256 for $($entry.Key): " +
                "expected $($entry.Value), got $actual"
            )
        }
    }
    $dlssLicenseRoot = Join-Path $buildRoot 'licenses\NVIDIA-Streamline'
    foreach ($notice in @(
        'license.txt',
        '3rd-party-licenses.md',
        'nvngx_dlss.license.txt'
    )) {
        $path = Join-Path $dlssLicenseRoot $notice
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "DLSS-enabled build is missing NVIDIA notice: $path"
        }
    }
    Write-Host 'Verified pinned NVIDIA Streamline 2.12.0 DLSS runtime.'
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
$objectsRoot = Join-Path $stageRoot 'objects'
New-Item -ItemType Directory -Path $objectsRoot -Force | Out-Null
foreach ($category in $defaultObjectCategories) {
    New-Item -ItemType Directory -Path (
        Join-Path $objectsRoot $category
    ) -Force | Out-Null
}
New-Item -ItemType Directory -Path (
    Join-Path $stageRoot 'Blender Map Tools'
) -Force | Out-Null
New-Item -ItemType Directory -Path (
    Join-Path $stageRoot 'Blender Map Tools\Source Tools'
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
if ($dlssEnabled) {
    foreach ($name in $dlssRuntimeHashes.Keys) {
        Copy-Item -LiteralPath (Join-Path $buildRoot $name) -Destination (
            Join-Path $stageRoot $name
        )
    }
    $nvidiaLicenseStage = Join-Path $stageRoot 'licenses\NVIDIA-Streamline'
    New-Item -ItemType Directory -Path $nvidiaLicenseStage -Force | Out-Null
    foreach ($notice in @(
        'license.txt',
        '3rd-party-licenses.md',
        'nvngx_dlss.license.txt'
    )) {
        Copy-Item -LiteralPath (
            Join-Path $buildRoot "licenses\NVIDIA-Streamline\$notice"
        ) -Destination (Join-Path $nvidiaLicenseStage $notice)
    }
}
Copy-Item -LiteralPath (Join-Path $repoRoot 'release\README.txt') -Destination (
    Join-Path $stageRoot 'README.txt'
)
Copy-Item -LiteralPath (Join-Path $repoRoot 'CUSTOM_MAPS.md') -Destination (
    Join-Path $stageRoot 'CUSTOM_MAPS.md'
)
Copy-Item -LiteralPath (Join-Path $repoRoot 'MULTIPLAYER.md') -Destination (
    Join-Path $stageRoot 'MULTIPLAYER.md'
)
Copy-Item -LiteralPath (
    Join-Path $repoRoot 'DLSS_SUPER_RESOLUTION.md'
) -Destination (
    Join-Path $stageRoot 'DLSS_SUPER_RESOLUTION.md'
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
Copy-Item -LiteralPath (
    Join-Path $repoRoot 'third_party\box3d\LICENSE'
) -Destination (
    Join-Path $stageRoot 'LICENSE-Box3D.txt'
)
Copy-Item -LiteralPath (
    Join-Path $repoRoot 'third_party\zlib\LICENSE'
) -Destination (
    Join-Path $stageRoot 'LICENSE-zlib.txt'
)
Copy-Item -LiteralPath (
    Join-Path $repoRoot 'third_party\zstd\LICENSE'
) -Destination (
    Join-Path $stageRoot 'LICENSE-zstd.txt'
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

$blenderToolsSource = Join-Path $repoRoot 'tools\blender_owned_map'
$blenderToolsStage = Join-Path $stageRoot 'Blender Map Tools\Source Tools'
$portableSourceExtensions = @(
    '.py', '.ps1', '.json', '.md', '.toml', '.yaml', '.yml'
)
Get-ChildItem -LiteralPath $blenderToolsSource -Recurse -File |
    Where-Object {
        $_.Extension.ToLowerInvariant() -in $portableSourceExtensions
    } |
    ForEach-Object {
        $relative = Get-PortableRelativePath `
            -BasePath $blenderToolsSource -TargetPath $_.FullName
        $destination = Join-Path $blenderToolsStage $relative
        New-Item -ItemType Directory -Path (
            Split-Path -Parent $destination
        ) -Force | Out-Null
        Copy-Item -LiteralPath $_.FullName -Destination $destination
    }

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
    '.dds', '.pyc', '.skateobj'
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
    $relative = (
        Get-PortableRelativePath `
            -BasePath $stageRoot -TargetPath $file.FullName
    ).Replace('\', '/')
    $isAllowedFirstPartyMap =
        $allowedFirstPartyMapFiles -contains $relative
    if ((-not $isAllowedFirstPartyMap -and
         $forbiddenExtensions -contains $file.Extension.ToLowerInvariant()) -or
        $forbiddenNames -contains $file.Name) {
        throw "Forbidden release payload detected: $($file.FullName)"
    }
    if ($file.Extension -ieq '.dll' -and
        ($file.Name.StartsWith(
            'sl.',
            [System.StringComparison]::OrdinalIgnoreCase
        ) -or $file.Name -ieq 'nvngx_dlss.dll') -and
        -not $dlssRuntimeHashes.Contains($file.Name)) {
        throw "Unexpected NVIDIA/Streamline plugin in release: $relative"
    }
}

if (-not $dlssEnabled) {
    foreach ($name in $dlssRuntimeHashes.Keys) {
        if (Test-Path -LiteralPath (Join-Path $stageRoot $name)) {
            throw "DLSS runtime was staged by a build with DLSS disabled: $name"
        }
    }
}

$checksums = foreach ($file in $stagedFiles | Sort-Object FullName) {
    $relative = (
        Get-PortableRelativePath `
            -BasePath $stageRoot -TargetPath $file.FullName
    ).Replace('\', '/')
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash
    "$hash  $relative"
}
$checksums | Set-Content -LiteralPath (
    Join-Path $stageRoot 'SHA256SUMS.txt'
) -Encoding ascii

Compress-Archive -LiteralPath $stageRoot -DestinationPath $archivePath `
    -CompressionLevel Optimal

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::OpenRead($archivePath)
try {
    $archiveEntries = [System.Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase
    )
    foreach ($entry in $archive.Entries) {
        [void]$archiveEntries.Add($entry.FullName)
    }
    foreach ($category in $defaultObjectCategories) {
        $expectedEntry = "$archiveBase/objects/$category/"
        if (-not $archiveEntries.Contains($expectedEntry)) {
            throw "Release archive is missing object category: $category"
        }
    }
    if ($dlssEnabled) {
        foreach ($name in $dlssRuntimeHashes.Keys) {
            if (-not $archiveEntries.Contains("$archiveBase/$name")) {
                throw "DLSS-enabled archive is missing runtime: $name"
            }
        }
        foreach ($notice in @(
            'license.txt',
            '3rd-party-licenses.md',
            'nvngx_dlss.license.txt'
        )) {
            $entry =
                "$archiveBase/licenses/NVIDIA-Streamline/$notice"
            if (-not $archiveEntries.Contains($entry)) {
                throw "DLSS-enabled archive is missing NVIDIA notice: $notice"
            }
        }
    }
} finally {
    $archive.Dispose()
}

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
