[CmdletBinding()]
param(
    [string]$BlenderExe = '',
    [string]$GameRoot = '',
    [switch]$ForceExport
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$workspace = Split-Path -Parent $PSScriptRoot
$repoRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot '..\..\..')
)
$buildRoot = Join-Path $repoRoot 'out\build\release'
$validatorBuildRoot = Join-Path (
    $repoRoot
) 'out\university-map-validator'
$runTimestamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$runRoot = Join-Path $repoRoot 'out\university-visual-check\prepared'
$logRoot = Join-Path $repoRoot (
    "out\university-visual-check\preparation-logs\$runTimestamp"
)
$preparationLog = Join-Path $logRoot 'preparation.log'

New-Item -ItemType Directory -Path $logRoot -Force | Out-Null
New-Item -ItemType Directory -Path $runRoot -Force | Out-Null
Start-Transcript -LiteralPath $preparationLog -Force | Out-Null

function Invoke-Checked {
    param(
        [Parameter(Mandatory)]
        [string]$FilePath,
        [Parameter(Mandatory)]
        [string[]]$Arguments,
        [Parameter(Mandatory)]
        [string]$Description
    )

    Write-Host ''
    Write-Host "== $Description =="
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Description failed with exit code $LASTEXITCODE"
    }
}

function Import-VisualStudioEnvironment {
    $candidates = @(
        'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat'
    )
    $vsDevCmd = $candidates |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($vsDevCmd)) {
        throw 'Visual Studio developer tools were not found.'
    }

    $environment = & cmd.exe /d /c (
        "`"$vsDevCmd`" -arch=x64 >nul && set"
    )
    if ($LASTEXITCODE -ne 0) {
        throw 'Visual Studio developer environment setup failed.'
    }
    foreach ($line in $environment) {
        $separator = $line.IndexOf('=')
        if ($separator -gt 0) {
            [Environment]::SetEnvironmentVariable(
                $line.Substring(0, $separator),
                $line.Substring($separator + 1),
                'Process'
            )
        }
    }
}

function Resolve-BlenderExecutable {
    if (-not [string]::IsNullOrWhiteSpace($BlenderExe)) {
        $candidate = [System.IO.Path]::GetFullPath($BlenderExe)
        if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            throw "Blender executable does not exist: $candidate"
        }
        return $candidate
    }

    $candidates = @(
        'C:\Program Files\Blender Foundation\Blender 5.0\blender.exe',
        'C:\Program Files\Blender Foundation\Blender 5.1\blender.exe'
    )
    $candidate = $candidates |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($candidate)) {
        throw (
            'Blender 5.x was not found. Pass -BlenderExe with its full path.'
        )
    }
    return $candidate
}

function Resolve-ValidatedGameRoot {
    if (-not [string]::IsNullOrWhiteSpace($GameRoot)) {
        $candidate = [System.IO.Path]::GetFullPath($GameRoot)
    } elseif (-not [string]::IsNullOrWhiteSpace(
            $env:SKATE3_GAME_DATA_ROOT
        )) {
        $candidate = [System.IO.Path]::GetFullPath(
            $env:SKATE3_GAME_DATA_ROOT
        )
    } else {
        $cache = Join-Path $buildRoot 'CMakeCache.txt'
        if (-not (Test-Path -LiteralPath $cache -PathType Leaf)) {
            throw (
                'No configured game root was found. Pass -GameRoot once; ' +
                'it must be the validated TU3 game directory.'
            )
        }
        $entry = Get-Content -LiteralPath $cache |
            Where-Object { $_ -like 'SKATE3_GAME_DATA_ROOT:PATH=*' } |
            Select-Object -First 1
        if ([string]::IsNullOrWhiteSpace($entry)) {
            throw 'CMakeCache.txt has no SKATE3_GAME_DATA_ROOT entry.'
        }
        $candidate = [System.IO.Path]::GetFullPath(
            $entry.Substring($entry.IndexOf('=') + 1)
        )
    }

    $required = @(
        (Join-Path $candidate 'default.xex'),
        (Join-Path $candidate 'default.xexp'),
        (Join-Path $candidate 'data\webkit\EAWebkit.xex'),
        (Join-Path $candidate 'data\webkit\EAWebkit.xexp')
    )
    foreach ($path in $required) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw (
                'The selected game root is not the complete validated TU3 ' +
                "layout; missing: $path"
            )
        }
    }
    return $candidate
}

function Assert-GeneratedCoverage {
    $functionsPath = Join-Path $repoRoot 'config\skate3_tu_functions.toml'
    $registerPath = Join-Path $repoRoot 'generated\skate3_register.cpp'
    foreach ($path in @($functionsPath, $registerPath)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw (
                'The validated generated cache is missing. Seed the complete ' +
                '1,727-root cache; do not run generate-all.'
            )
        }
    }

    $required = [System.Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase
    )
    foreach ($line in Get-Content -LiteralPath $functionsPath) {
        if ($line -match '^\s*"(0x[0-9A-Fa-f]{8})"\s*=') {
            [void]$required.Add($matches[1])
        }
    }
    $registered = [System.Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase
    )
    $contents = Get-Content -LiteralPath $registerPath -Raw
    foreach ($match in [regex]::Matches(
            $contents,
            'SetFunction\((0x[0-9A-Fa-f]{8}),'
        )) {
        [void]$registered.Add($match.Groups[1].Value)
    }
    $missing = @(
        $required |
            Where-Object { -not $registered.Contains($_) }
    )
    if ($required.Count -ne 1727 -or $missing.Count -ne 0) {
        throw (
            'Generated TU3 coverage is invalid: required={0}, missing={1}.' -f
            $required.Count, $missing.Count
        )
    }
    Write-Host 'Verified TU3 function coverage: 1727/1727 roots registered.'
}

function Test-OutputStale {
    param(
        [Parameter(Mandatory)]
        [string]$Output,
        [Parameter(Mandatory)]
        [object[]]$Inputs
    )

    if (-not (Test-Path -LiteralPath $Output -PathType Leaf)) {
        return $true
    }
    $outputTime = (Get-Item -LiteralPath $Output).LastWriteTimeUtc
    foreach ($inputItem in $Inputs) {
        if ($inputItem.LastWriteTimeUtc -gt $outputTime) {
            return $true
        }
    }
    return $false
}

function Assert-Equal {
    param(
        [Parameter(Mandatory)]
        [string]$Name,
        [Parameter(Mandatory)]
        [object]$Actual,
        [Parameter(Mandatory)]
        [object]$Expected
    )

    if ([string]$Actual -cne [string]$Expected) {
        throw (
            "University integrity mismatch for ${Name}: " +
            "actual=$Actual expected=$Expected"
        )
    }
}

try {
    $gitMarker = Get-Item -LiteralPath (Join-Path $repoRoot '.git') -Force
    if ($gitMarker.PSIsContainer) {
        throw (
            'Refusing to run from a primary checkout. Use the dedicated ' +
            'University linked worktree.'
        )
    }

    $blender = Resolve-BlenderExecutable
    $gameData = Resolve-ValidatedGameRoot
    $baseBlend = Join-Path $workspace 'blender\DIST_University.blend'
    $ownedBlend = Join-Path (
        $workspace
    ) 'blender\DIST_University_Owned.blend'
    $package = Join-Path (
        $workspace
    ) 'intermediate\university\University.skate'
    $prepareOwned = Join-Path (
        $workspace
    ) 'blender\prepare_university_owned.py'
    $validateBlend = Join-Path (
        $workspace
    ) 'blender\validate_university_blend.py'
    $exporter = Join-Path (
        $repoRoot
    ) 'tools\blender_owned_map\export_skate.py'
    $analyzer = Join-Path (
        $repoRoot
    ) 'tools\blender_owned_map\analyze_skate.py'
    $grindVerifier = Join-Path (
        $workspace
    ) 'tools\verify_university_grinds.py'
    $collisionVerifier = Join-Path (
        $workspace
    ) 'tools\verify_university_collision.py'
    $materialVerifier = Join-Path (
        $workspace
    ) 'tools\verify_university_material_channels.py'
    $collisionProbeBuilder = Join-Path (
        $workspace
    ) 'tools\build_university_collision_probe.py'
    $extractionManifest = Join-Path (
        $workspace
    ) 'intermediate\university\manifest.json'
    $collisionProbe = Join-Path (
        $workspace
    ) 'intermediate\university\University.spawn-collision.rwcmset'
    $expectedPath = Join-Path (
        $workspace
    ) 'schemas\university_expected.json'
    $buildBase = Join-Path $PSScriptRoot 'Build-UniversityBlend.ps1'

    Write-Host "Dedicated worktree: $repoRoot"
    Write-Host "Prepared output:    $runRoot"
    Write-Host "Preparation log:    $preparationLog"
    Write-Host "Validated TU3 data: $gameData"
    Write-Host "Blender:            $blender"

    Assert-GeneratedCoverage

    $baseInputs = @(
        (Get-Item -LiteralPath $buildBase)
        (Get-Item -LiteralPath (
            Join-Path $workspace 'tools\prepare_university.py'
        ))
        (Get-Item -LiteralPath (
            Join-Path $workspace 'tools\prepare_hawaiian_dream.py'
        ))
        (Get-Item -LiteralPath (
            Join-Path $workspace 'blender\import_university.py'
        ))
        (Get-Item -LiteralPath (
            Join-Path $workspace 'blender\import_hawaiian_dream.py'
        ))
        (Get-Item -LiteralPath (
            Join-Path $workspace 'tools\retail_collision_mesh.py'
        ))
        (Get-Item -LiteralPath (
            Join-Path $workspace 'tools\retail_grind_splines.py'
        ))
    )
    if ($ForceExport -or (
            Test-OutputStale -Output $baseBlend -Inputs $baseInputs
        )) {
        Write-Host (
            'Base University blend is missing or stale; rebuilding ' +
            'the retail extraction.'
        )
        & $buildBase -BlenderExe $blender
        if ($LASTEXITCODE -ne 0) {
            throw "University base blend rebuild failed: $LASTEXITCODE"
        }
    }

    Invoke-Checked -FilePath $blender -Arguments @(
        '--background',
        $baseBlend,
        '--python',
        $validateBlend
    ) -Description 'Validate University retail material bindings and alpha'

    $ownedInputs = @(
        (Get-Item -LiteralPath $baseBlend)
        (Get-Item -LiteralPath $prepareOwned)
    )
    if ($ForceExport -or (
            Test-OutputStale -Output $ownedBlend -Inputs $ownedInputs
        )) {
        Invoke-Checked -FilePath $blender -Arguments @(
            '--background',
            $baseBlend,
            '--python',
            $prepareOwned,
            '--',
            $ownedBlend
        ) -Description 'Prepare University owned-world Blender scene'
    } else {
        Write-Host "Owned scene is current: $ownedBlend"
    }

    $exportInputs = @(
        (Get-Item -LiteralPath $ownedBlend)
    ) + @(
        Get-ChildItem -LiteralPath (
            Join-Path $repoRoot (
                'tools\blender_owned_map\owned_world_material_addon'
            )
        ) -Recurse -File -Filter '*.py'
    ) + @(
        (Get-Item -LiteralPath $exporter)
    )
    if ($ForceExport -or (
            Test-OutputStale -Output $package -Inputs $exportInputs
        )) {
        $exportArguments = @(
            '--background',
            $ownedBlend,
            '--python',
            $exporter,
            '--',
            $package
        )
        if ($ForceExport) {
            $exportArguments += '--force'
        }
        Invoke-Checked -FilePath $blender -Arguments $exportArguments `
            -Description 'Export University SKATE v11 package'
    } else {
        Write-Host "SKATE package is current: $package"
    }

    $analysisPath = Join-Path $logRoot 'University.analysis.json'
    Write-Host ''
    Write-Host '== Analyze University package =='
    & python $analyzer $package '--json' > $analysisPath
    if ($LASTEXITCODE -ne 0) {
        throw "University package analysis failed: $LASTEXITCODE"
    }
    $actual = Get-Content -LiteralPath $analysisPath -Raw |
        ConvertFrom-Json
    $expected = Get-Content -LiteralPath $expectedPath -Raw |
        ConvertFrom-Json
    Invoke-Checked -FilePath 'python' -Arguments @(
        $grindVerifier,
        $extractionManifest,
        $package
    ) -Description 'Verify exact retail grind byte round trip'
    Invoke-Checked -FilePath 'python' -Arguments @(
        $collisionVerifier,
        $extractionManifest,
        $package,
        '--expected',
        $expectedPath
    ) -Description 'Verify retail collision geometry and packed surfaces'
    Invoke-Checked -FilePath 'python' -Arguments @(
        $materialVerifier,
        $extractionManifest,
        $package,
        '--expected',
        $expectedPath
    ) -Description 'Verify conservative retail normal-map transport'
    Invoke-Checked -FilePath 'python' -Arguments @(
        $collisionProbeBuilder,
        $extractionManifest,
        $collisionProbe
    ) -Description (
        'Build exact retail Mega Park collision comparison archive'
    )

    Assert-Equal 'format version' $actual.version $expected.format_version
    Assert-Equal 'map name' $actual.map_name $expected.map_name
    Assert-Equal 'file bytes' $actual.file_bytes $expected.file_bytes
    Assert-Equal (
        'decoded texture bytes'
    ) $actual.texture_decoded_bytes $expected.texture_decoded_bytes
    Assert-Equal (
        'maximum visual index'
    ) $actual.maximum_visual_index $expected.maximum_visual_index
    foreach ($name in @(
            'materials',
            'textures',
            'vertices',
            'indices',
            'collision_triangles',
            'grind_rails',
            'native_grind_segments',
            'hinged_doors',
            'local_lights',
            'npc_routes'
        )) {
        Assert-Equal "count $name" $actual.counts.$name `
            $expected.counts.$name
    }
    foreach ($name in @('opaque', 'mask', 'blend')) {
        Assert-Equal "material alpha mode $name" `
            $actual.material_alpha_modes.$name `
            $expected.material_alpha_modes.$name
    }
    foreach ($name in @(
            'semantic_metadata_sha256',
            'materials_sha256',
            'decoded_textures_sha256',
            'expanded_visual_triangles_sha256',
            'expanded_visual_triangles_1e6_sha256',
            'collision_sha256',
            'grind_rails_sha256',
            'authored_features_sha256'
        )) {
        Assert-Equal "integrity $name" $actual.integrity.$name `
            $expected.integrity.$name
    }
    for ($index = 0; $index -lt 3; ++$index) {
        $minimumDelta = [math]::Abs(
            [double]$actual.integrity.bounds_min[$index] -
            [double]$expected.bounds_min[$index]
        )
        $maximumDelta = [math]::Abs(
            [double]$actual.integrity.bounds_max[$index] -
            [double]$expected.bounds_max[$index]
        )
        if ($minimumDelta -gt 0.0001 -or $maximumDelta -gt 0.0001) {
            throw "University bounds changed on axis $index."
        }
    }
    Write-Host 'University package integrity manifest: PASS'

    Import-VisualStudioEnvironment
    $clang = 'C:/Program Files/LLVM/bin/clang.exe'
    $clangCxx = 'C:/Program Files/LLVM/bin/clang++.exe'
    foreach ($compiler in @($clang, $clangCxx)) {
        if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) {
            throw "Required Clang compiler is missing: $compiler"
        }
    }
    $cmakeGameRoot = $gameData.Replace('\', '/')
    Invoke-Checked -FilePath 'cmake' -Arguments @(
        '--preset',
        'release',
        "-DSKATE3_GAME_DATA_ROOT=$cmakeGameRoot",
        "-DCMAKE_C_COMPILER=$clang",
        "-DCMAKE_CXX_COMPILER=$clangCxx"
    ) -Description 'Configure dedicated University game build'
    $cmakeSourceRoot = $repoRoot.Replace('\', '/')
    Invoke-Checked -FilePath 'cmake' -Arguments @(
        "-DSKATE3_SOURCE_DIR=$cmakeSourceRoot",
        '-P',
        (Join-Path $repoRoot 'cmake\ApplySkate3CodegenPatches.cmake')
    ) -Description 'Apply generated native instrumentation patches'
    Invoke-Checked -FilePath 'cmake' -Arguments @(
        '--build',
        '--preset',
        'release',
        '--target',
        'skate3'
    ) -Description 'Build dedicated University game'
    Invoke-Checked -FilePath 'cmake' -Arguments @(
        '-S',
        (Join-Path $repoRoot 'owned\world'),
        '-B',
        $validatorBuildRoot,
        '-G',
        'Ninja',
        '-DCMAKE_BUILD_TYPE=Release',
        "-DCMAKE_C_COMPILER=$clang",
        "-DCMAKE_CXX_COMPILER=$clangCxx",
        '-DSKATE_OWNED_WORLD_BUILD_TESTS=ON'
    ) -Description 'Configure offline University package validator'
    Invoke-Checked -FilePath 'cmake' -Arguments @(
        '--build',
        $validatorBuildRoot,
        '--target',
        'skate_owned_map_validate'
    ) -Description 'Build offline University package validator'

    $validator = Join-Path $validatorBuildRoot 'skate_owned_map_validate.exe'
    if (-not (Test-Path -LiteralPath $validator -PathType Leaf)) {
        throw "Offline package validator was not built: $validator"
    }
    $validationPath = Join-Path $logRoot 'University.validation.txt'
    Write-Host ''
    Write-Host '== Validate loader, render world, and collision world =='
    $validationOutput = @(
        & $validator $package '--compile-world'
    )
    if ($LASTEXITCODE -ne 0) {
        throw "University engine-side validation failed: $LASTEXITCODE"
    }
    $validationOutput | Tee-Object -FilePath $validationPath
    $validationText = $validationOutput -join "`n"
    $compiled = $expected.compiled_world
    $requiredPatterns = @(
        (
            'SKATE_RENDER_WORLD_OK source_triangles={0} ' +
            'output_triangles={1} chunks={2}'
        ) -f $compiled.render_source_triangles,
            $compiled.render_output_triangles,
            $compiled.render_chunks
        (
            'SKATE_GRIND_WORLD_OK rails={0} segments={1} bytes={2}'
        ) -f $compiled.grind_rails,
            $compiled.grind_segments,
            $compiled.grind_bytes
    )
    if ($compiled.collision_mode -eq 'continuous') {
        $requiredPatterns += (
            'SKATE_COLLISION_WORLD_OK mode=continuous triangles={0} ' +
            'vertices={1} clusters={2} bytes={3}'
        ) -f $compiled.collision_triangles,
            $compiled.collision_vertices,
            $compiled.collision_clusters,
            $compiled.collision_bytes
    } else {
        $requiredPatterns += (
            'SKATE_COLLISION_WORLD_OK mode={0} cell_size={1} chunks={2} ' +
            'triangles={3}'
        ) -f $compiled.collision_mode,
            $compiled.collision_cell_size,
            $compiled.collision_chunks,
            $compiled.collision_triangles
    }
    foreach ($pattern in $requiredPatterns) {
        if (-not $validationText.Contains($pattern)) {
            throw "Compiled-world integrity line is missing: $pattern"
        }
    }
    Write-Host 'Engine loader/render/collision validation: PASS'

    $stagedExecutable = Join-Path $runRoot 'skate3.exe'
    $stagedRuntime = Join-Path $runRoot 'rexruntime.dll'
    $stagedMapRoot = Join-Path $runRoot 'owned_maps'
    $stagedPackage = Join-Path $stagedMapRoot 'University.skate'
    $stagedCollisionProbe = Join-Path (
        $stagedMapRoot
    ) 'University.spawn-collision.rwcmset'
    New-Item -ItemType Directory -Path $stagedMapRoot -Force | Out-Null
    New-Item -ItemType Directory -Path (
        Join-Path $runRoot 'maps'
    ) -Force | Out-Null
    New-Item -ItemType File -Path (
        Join-Path $runRoot 'portable.txt'
    ) -Force | Out-Null
    Copy-Item -LiteralPath (
        Join-Path $buildRoot 'skate3.exe'
    ) -Destination $stagedExecutable -Force
    Copy-Item -LiteralPath (
        Join-Path $buildRoot 'rexruntime.dll'
    ) -Destination $stagedRuntime -Force
    Copy-Item -LiteralPath $package -Destination $stagedPackage -Force
    Copy-Item -LiteralPath $collisionProbe `
        -Destination $stagedCollisionProbe -Force
    $stagedGame = Join-Path $runRoot 'game'
    if (Test-Path -LiteralPath $stagedGame) {
        $existingGame = Get-Item -LiteralPath $stagedGame -Force
        $existingTarget = [System.IO.Path]::GetFullPath(
            [string]$existingGame.Target
        )
        if ($existingTarget -cne $gameData) {
            if (-not ($existingGame.Attributes -band (
                        [System.IO.FileAttributes]::ReparsePoint
                    ))) {
                throw (
                    'Refusing to replace a non-junction prepared game path: ' +
                    $stagedGame
                )
            }
            Remove-Item -LiteralPath $stagedGame -Force
            New-Item -ItemType Junction -Path $stagedGame `
                -Target $gameData | Out-Null
        }
    } else {
        New-Item -ItemType Junction -Path $stagedGame `
            -Target $gameData | Out-Null
    }

    $seedRoot = Join-Path $env:APPDATA 'skate3'
    if (Test-Path -LiteralPath $seedRoot -PathType Container) {
        Get-ChildItem -LiteralPath $seedRoot -Directory |
            Where-Object Name -Match '^[0-9A-Fa-f]{16}$' |
            ForEach-Object {
                $seedDestination = Join-Path $runRoot $_.Name
                New-Item -ItemType Directory -Path $seedDestination `
                    -Force | Out-Null
                Get-ChildItem -LiteralPath $_.FullName -Force |
                    Copy-Item -Destination $seedDestination -Recurse -Force
            }
    }

    $commit = (& git -C $repoRoot rev-parse HEAD).Trim()
    $branch = (& git -C $repoRoot branch --show-current).Trim()
    $stageManifest = [ordered]@{
        timestamp = $runTimestamp
        worktree = $repoRoot
        branch = $branch
        commit = $commit
        game_root = $gameData
        executable_sha256 = (
            Get-FileHash -LiteralPath $stagedExecutable -Algorithm SHA256
        ).Hash
        runtime_sha256 = (
            Get-FileHash -LiteralPath $stagedRuntime -Algorithm SHA256
        ).Hash
        map_sha256 = (
            Get-FileHash -LiteralPath $stagedPackage -Algorithm SHA256
        ).Hash
        collision_probe_sha256 = (
            Get-FileHash -LiteralPath $stagedCollisionProbe `
                -Algorithm SHA256
        ).Hash
        expected_contract = $expectedPath
        map_analysis = $analysisPath
        map_validation = $validationPath
        collision_source = (
            'eight untouched retail RenderWare ClusteredMesh resources ' +
            'nearest the Super Ultra Mega Park spawn'
        )
    }
    $manifestJson = $stageManifest | ConvertTo-Json -Depth 4
    $manifestJson | Set-Content -LiteralPath (
        Join-Path $logRoot 'prepared-manifest.json'
    ) -Encoding UTF8
    $manifestJson | Set-Content -LiteralPath (
        Join-Path $runRoot 'prepared-manifest.json'
    ) -Encoding UTF8

    Write-Host ''
    Write-Host "Prepared visual-check build: $runRoot"
    Write-Host "Preparation logs:           $logRoot"
    Write-Host 'Offline preparation complete. The game was NOT launched.'
} catch {
    Write-Error $_
    exit 1
} finally {
    try {
        Stop-Transcript | Out-Null
    } catch {
    }
}
