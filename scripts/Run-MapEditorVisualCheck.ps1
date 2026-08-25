param(
    [string]$GameDataRoot = $env:SKATE3_GAME_DATA_ROOT,
    [string]$CodegenGameDataRoot = $env:SKATE3_CODEGEN_GAME_DATA_ROOT,
    [string]$TrustedGeneratedRoot = $env:SKATE3_TRUSTED_GENERATED_ROOT,
    [switch]$UseGeneratedEditorMap,
    [switch]$VerifyOnly,
    [switch]$FullRebuild
)

$ErrorActionPreference = 'Stop'
$repoRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot '..')
)
$buildRoot = Join-Path $repoRoot 'out\build\map-editor-release-clang'
$worldBuildRoot = Join-Path $repoRoot 'out\build\map-editor-world-release'
$mapFileName = if ($UseGeneratedEditorMap) {
    'map_editor_mvp.skate'
} else {
    'blender_bake_showcase.skate'
}
$mapPath = Join-Path $repoRoot "maps\$mapFileName"
$objectRoot = Join-Path $repoRoot 'objects'
$testObjectPath = Join-Path $objectRoot 'test_grind_ledge.skateobj'
$clang = 'C:\Program Files\LLVM\bin\clang.exe'
$clangCxx = 'C:\Program Files\LLVM\bin\clang++.exe'

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

function Invoke-Checked {
    param(
        [string]$FilePath,
        [string[]]$Arguments,
        [string]$Description
    )
    Write-Host "== $Description =="
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Description failed with exit code $LASTEXITCODE."
    }
}

function Get-MissingTuFunctions {
    $tuFunctionsPath =
        Join-Path $repoRoot 'config\skate3_tu_functions.toml'
    $registerPath = Join-Path $repoRoot 'generated\skate3_register.cpp'
    if (-not (Test-Path -LiteralPath $tuFunctionsPath -PathType Leaf) -or
        -not (Test-Path -LiteralPath $registerPath -PathType Leaf)) {
        return @('<generated registration unavailable>')
    }

    $required = [System.Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase
    )
    foreach ($line in Get-Content -LiteralPath $tuFunctionsPath) {
        if ($line -match '^\s*"(0x[0-9A-Fa-f]{8})"\s*=') {
            [void]$required.Add($matches[1])
        }
    }
    $registered = [System.Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase
    )
    $registerContents = Get-Content -LiteralPath $registerPath -Raw
    foreach ($match in [regex]::Matches(
            $registerContents,
            'SetFunction\((0x[0-9A-Fa-f]{8}),'
        )) {
        [void]$registered.Add($match.Groups[1].Value)
    }
    return @(
        $required |
            Where-Object { -not $registered.Contains($_) } |
            Sort-Object
    )
}

function Get-GeneratedFingerprint {
    param([string]$Root)

    if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
        return '<missing>'
    }
    $resolvedRoot = [System.IO.Path]::GetFullPath($Root)
    $lines = foreach ($file in (
            Get-ChildItem -LiteralPath $resolvedRoot -Recurse -File |
                Sort-Object FullName
        )) {
        $relativePath = $file.FullName.Substring(
            $resolvedRoot.Length + 1
        ).Replace('\', '/')
        $hash = (
            Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256
        ).Hash
        "$relativePath|$($file.Length)|$hash"
    }
    return $lines -join "`n"
}

function Read-EditorStatus {
    try {
        $pipe = [System.IO.Pipes.NamedPipeClientStream]::new(
            '.', 'Skate3InputLab',
            [System.IO.Pipes.PipeDirection]::InOut
        )
        $pipe.Connect(300)
        $pipe.ReadMode = [System.IO.Pipes.PipeTransmissionMode]::Message
        $writer = [System.IO.StreamWriter]::new(
            $pipe, [System.Text.Encoding]::ASCII, 1024, $true
        )
        $writer.AutoFlush = $true
        $reader = [System.IO.StreamReader]::new(
            $pipe, [System.Text.Encoding]::UTF8, $false, 4096, $true
        )
        $writer.WriteLine('STATUS')
        $response = $reader.ReadLine()
        $reader.Dispose()
        $writer.Dispose()
        $pipe.Dispose()
        return $response
    } catch {
        return $null
    }
}

try {
    Set-Location -LiteralPath $repoRoot
    if ([string]::IsNullOrWhiteSpace($GameDataRoot)) {
        $GameDataRoot =
            'C:\Users\Daddy\Documents\SK8 Engine - Latest Release\game'
    }
    $GameDataRoot = [System.IO.Path]::GetFullPath($GameDataRoot)
    if ([string]::IsNullOrWhiteSpace($CodegenGameDataRoot)) {
        $CodegenGameDataRoot = 'C:\sk83_recomp\assets'
    }
    $CodegenGameDataRoot =
        [System.IO.Path]::GetFullPath($CodegenGameDataRoot)
    if ([string]::IsNullOrWhiteSpace($TrustedGeneratedRoot)) {
        $TrustedGeneratedRoot = (
            'C:\Users\Daddy\Documents\' +
            'Skate3CustomEngineLayer-Release-preview14\Source\generated'
        )
    }
    $TrustedGeneratedRoot =
        [System.IO.Path]::GetFullPath($TrustedGeneratedRoot)
    foreach ($required in @(
            (Join-Path $GameDataRoot 'default.xex'),
            (Join-Path $GameDataRoot 'data\webkit\EAWebkit.xex'),
            (Join-Path $CodegenGameDataRoot 'default.xex'),
            (Join-Path $CodegenGameDataRoot 'data\webkit\EAWebkit.xex'),
            (Join-Path $TrustedGeneratedRoot 'sources.cmake'),
            (Join-Path $TrustedGeneratedRoot 'skate3_register.cpp'),
            (Join-Path $TrustedGeneratedRoot (
                'eawebkit\eawebkit_register.cpp'
            )),
            $clang,
            $clangCxx,
            $mapPath
        )) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "Required visual-check input is missing: $required"
        }
    }

    $head = (& git rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or
        $head -ne '8c0769b3ac7923f89c021da6ca60d20361b12935') {
        throw "Wrong map-editor baseline: $head"
    }
    $runtimeHead = (& git -C third_party/rexglue-sdk rev-parse HEAD).Trim()
    if ($runtimeHead -ne '3818a4832e2bd03ff5a1a2e93c52b855dc181096') {
        throw "Wrong runtime submodule baseline: $runtimeHead"
    }

    $codegenXex = Join-Path $CodegenGameDataRoot 'default.xex'
    $codegenXexHash = (
        Get-FileHash -LiteralPath $codegenXex -Algorithm SHA256
    ).Hash
    $expectedCodegenXexHash =
        '4E49F302896A9BEC3B82CD19D5561247F1A50D6040E1A000F02299CE5B7CB7E9'
    if ($codegenXexHash -ne $expectedCodegenXexHash) {
        throw (
            "Codegen requires the expanded preview.14 retail default.xex. " +
            "Expected SHA256 $expectedCodegenXexHash at $codegenXex, got " +
            "$codegenXexHash. The packaged release game/default.xex is a " +
            "runtime input and must not be used for host-code generation."
        )
    }
    $trustedRegisterHash = (
        Get-FileHash -LiteralPath (
            Join-Path $TrustedGeneratedRoot 'skate3_register.cpp'
        ) -Algorithm SHA256
    ).Hash
    $expectedTrustedRegisterHash =
        '348DFB95B0BF81ADE9C970CAAF28C36677B726FFF4EA4207468B0F18AE3EA271'
    if ($trustedRegisterHash -ne $expectedTrustedRegisterHash) {
        throw (
            "The trusted preview.14 generated cache does not match the " +
            "reviewed release boundary. Expected skate3_register.cpp " +
            "SHA256 $expectedTrustedRegisterHash, got $trustedRegisterHash."
        )
    }
    $trustedFingerprint =
        Get-GeneratedFingerprint -Root $TrustedGeneratedRoot
    $generatedRoot = Join-Path $repoRoot 'generated'
    $currentFingerprint = Get-GeneratedFingerprint -Root $generatedRoot
    if ($currentFingerprint -cne $trustedFingerprint) {
        Write-Host (
            'Restoring the vetted preview.14 generated host-code cache. ' +
            'The current cache is absent, stale, or was produced from an ' +
            'unsupported input.'
        )
        [System.IO.Directory]::CreateDirectory($generatedRoot) | Out-Null
        Get-ChildItem -LiteralPath $TrustedGeneratedRoot -Force |
            Copy-Item -Destination $generatedRoot -Recurse -Force
        $currentFingerprint = Get-GeneratedFingerprint -Root $generatedRoot
        if ($currentFingerprint -cne $trustedFingerprint) {
            throw 'Failed to restore the exact preview.14 generated cache.'
        }
    }
    Write-Host 'Verified exact preview.14 generated host-code cache.'

    Import-VisualStudioEnvironment
    if ($FullRebuild -and (Test-Path -LiteralPath $buildRoot)) {
        $resolvedBuild = [System.IO.Path]::GetFullPath($buildRoot)
        $expectedParent = [System.IO.Path]::GetFullPath(
            (Join-Path $repoRoot 'out\build')
        ) + [System.IO.Path]::DirectorySeparatorChar
        if (-not $resolvedBuild.StartsWith(
                $expectedParent,
                [System.StringComparison]::OrdinalIgnoreCase
            )) {
            throw "Refusing full rebuild outside worktree build root."
        }
        Remove-Item -LiteralPath $resolvedBuild -Recurse -Force
    }

    $cmakeGameRoot = $CodegenGameDataRoot.Replace('\', '/')
    $buildNinja = Join-Path $buildRoot 'build.ninja'
    $cachePath = Join-Path $buildRoot 'CMakeCache.txt'
    $configuredGameRoot = $null
    $configuredRuntimeTitleUpdate = $null
    if (Test-Path -LiteralPath $cachePath -PathType Leaf) {
        $cacheMatch = Select-String -LiteralPath $cachePath -Pattern (
            '^SKATE3_GAME_DATA_ROOT:PATH=(.*)$'
        ) | Select-Object -First 1
        if ($null -ne $cacheMatch) {
            $configuredGameRoot = [System.IO.Path]::GetFullPath(
                $cacheMatch.Matches[0].Groups[1].Value
            )
        }
        $titleUpdateMatch =
            Select-String -LiteralPath $cachePath -Pattern (
                '^SKATE3_RUNTIME_TITLE_UPDATE:BOOL=(.*)$'
            ) | Select-Object -First 1
        if ($null -ne $titleUpdateMatch) {
            $configuredRuntimeTitleUpdate =
                $titleUpdateMatch.Matches[0].Groups[1].Value
        }
    }
    $needsConfigure =
        -not (Test-Path -LiteralPath $buildNinja -PathType Leaf) -or
        [string]::IsNullOrWhiteSpace($configuredGameRoot) -or
        $configuredRuntimeTitleUpdate -ne 'ON' -or
        -not $configuredGameRoot.Equals(
            $CodegenGameDataRoot,
            [System.StringComparison]::OrdinalIgnoreCase
        )
    if ($needsConfigure) {
        Invoke-Checked -FilePath 'cmake' -Arguments @(
            '-S', $repoRoot,
            '-B', $buildRoot,
            '-G', 'Ninja',
            '-DCMAKE_BUILD_TYPE=Release',
            "-DCMAKE_C_COMPILER=$clang",
            "-DCMAKE_CXX_COMPILER=$clangCxx",
            '-DREXSDK_DIR=third_party/rexglue-sdk',
            "-DSKATE3_GAME_DATA_ROOT=$cmakeGameRoot",
            '-DSKATE3_RUNTIME_TITLE_UPDATE=ON',
            '-DREXGLUE_USE_VULKAN=ON'
        ) -Description (
            'Configure worktree-local Release build with verified codegen input'
        )
    }
    $missingTuFunctions = @(Get-MissingTuFunctions)
    if ($missingTuFunctions.Count -ne 0) {
        $sample = (
            $missingTuFunctions | Select-Object -First 20
        ) -join ', '
        throw (
            "Generated TU3 function coverage is incomplete: " +
            "$($missingTuFunctions.Count) roots missing. " +
            "First missing roots: $sample"
        )
    }
    Write-Host 'Verified TU3 function coverage before launch.'
    Invoke-Checked -FilePath 'cmake' -Arguments @(
        '--build', $buildRoot,
        '--target', 'skate3',
        '--parallel'
    ) -Description 'Incrementally build map-editor executable'

    if (-not (Test-Path -LiteralPath (
                Join-Path $worldBuildRoot 'build.ninja'
            ) -PathType Leaf)) {
        Invoke-Checked -FilePath 'cmake' -Arguments @(
            '-S', (Join-Path $repoRoot 'owned\world'),
            '-B', $worldBuildRoot,
            '-G', 'Ninja',
            '-DCMAKE_BUILD_TYPE=Release',
            '-DSKATE_OWNED_WORLD_BUILD_TESTS=ON'
        ) -Description 'Configure worktree-local owned-world tests'
    }
    Invoke-Checked -FilePath 'cmake' -Arguments @(
        '--build', $worldBuildRoot,
        '--parallel'
    ) -Description 'Incrementally build owned-world tests'
    Invoke-Checked -FilePath 'ctest' -Arguments @(
        '--test-dir', $worldBuildRoot,
        '--output-on-failure'
    ) -Description 'Run owned-world tests'
    Invoke-Checked -FilePath (
        Join-Path $worldBuildRoot 'skate_owned_map_validate.exe'
    ) -Arguments @(
        $mapPath, '--compile-world'
    ) -Description "Validate and compile visual-check map $mapFileName"
    Invoke-Checked -FilePath (
        Join-Path $worldBuildRoot 'skate_owned_map_validate.exe'
    ) -Arguments @(
        $testObjectPath, '--object-profile', '--compile-world'
    ) -Description 'Validate grindable SKATEOBJ spawn-menu test asset'

    if ($VerifyOnly) {
        Write-Host ''
        Write-Host (
            'Map-editor visual-check preflight passed; game launch skipped.'
        )
        exit 0
    }

    $timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $runRoot = Join-Path $repoRoot "out\map-editor-runs\$timestamp"
    $logRoot = Join-Path $runRoot 'logs'
    $stagedMapRoot = Join-Path $runRoot 'owned_maps'
    $stagedObjectRoot = Join-Path $runRoot 'objects'
    New-Item -ItemType Directory -Path $logRoot -Force | Out-Null
    New-Item -ItemType Directory -Path $stagedMapRoot -Force | Out-Null
    New-Item -ItemType Directory -Path $stagedObjectRoot -Force | Out-Null
    New-Item -ItemType File -Path (
        Join-Path $runRoot 'portable.txt'
    ) -Force | Out-Null
    Copy-Item -LiteralPath (
        Join-Path $buildRoot 'skate3.exe'
    ) -Destination $runRoot -Force
    Copy-Item -LiteralPath (
        Join-Path $buildRoot 'rexruntime.dll'
    ) -Destination $runRoot -Force
    Copy-Item -LiteralPath $mapPath -Destination (
        Join-Path $stagedMapRoot $mapFileName
    ) -Force
    Get-ChildItem -LiteralPath $objectRoot -Filter '*.skateobj' -File |
        Copy-Item -Destination $stagedObjectRoot -Force
    New-Item -ItemType Junction -Path (
        Join-Path $runRoot 'game'
    ) -Target $GameDataRoot | Out-Null

    $seedRoot = Join-Path $env:APPDATA 'skate3'
    if (Test-Path -LiteralPath $seedRoot -PathType Container) {
        Get-ChildItem -LiteralPath $seedRoot -Directory |
            Where-Object Name -Match '^[0-9A-Fa-f]{16}$' |
            ForEach-Object {
                Copy-Item -LiteralPath $_.FullName -Destination $runRoot `
                    -Recurse -Force
            }
    }

    $eventLog = Join-Path $logRoot "map-editor-events-$timestamp.log"
    $telemetryLog =
        Join-Path $logRoot "map-editor-telemetry-$timestamp.log"
    $arguments = @(
        '--fullscreen=false',
        '--window_width=1280',
        '--window_height=720',
        '--skate3_direct_boot=true',
        '--skate3_input_lab=true',
        '--skate3_mechanics_sandbox=true',
        '--skate3_mechanics_sandbox_visual_map=true',
        '--skate3_mechanics_sandbox_native_collision=true',
        '--skate3_mechanics_sandbox_native_collision_replace_retail=true',
        '--skate3_native_render=true',
        '--skate3_native_render_scene=true',
        '--skate3_native_render_scene_perf_log=true',
        '--skate3_native_render_scene_perf_interval=60',
        "--log_file=$eventLog",
        '--log_level=info'
    )
    $arguments | Set-Content -LiteralPath (
        Join-Path $runRoot 'launch-arguments.txt'
    ) -Encoding UTF8
    $env:SKATE3_OWNED_MAP = "owned_maps/$mapFileName"

    Write-Host ''
    Write-Host "Prepared run:       $runRoot"
    Write-Host "Test map:           $mapFileName"
    Write-Host 'Test object:        objects/test_grind_ledge.skateobj'
    Write-Host "Event log:          $eventLog"
    Write-Host "Telemetry log:      $telemetryLog"
    Write-Host ''
    Write-Host 'Controls: G editor; hold RMB to hide/capture mouse-look; WASD move;'
    Write-Host 'Space up; Q/C down; Shift fast; Ctrl slow; E opens the object list;'
    Write-Host 'LMB selects; drag red/green/blue arrows to move on X/Y/Z;'
    Write-Host 'drag the matching coloured rings to rotate; release LMB to commit; G exits.'
    Write-Host 'F6 screenshot; Alt+F6 draw-distance marker.'
    Write-Host ''
    if ($UseGeneratedEditorMap) {
        Write-Host 'Check: the skater must stand on EditorGround immediately; do not move'
        Write-Host 'anything to activate collision. Enter G, confirm the skater stays visible,'
        Write-Host 'hold RMB and move the mouse, then release RMB and confirm the cursor returns.'
        Write-Host 'Select MoveMe_OrangeBlock and confirm only a subtle cyan outer contour appears. Drag an'
        Write-Host 'axis arrow, rotate with a ring, then G-exit and collide at its new pose.'
        Write-Host 'Grind all four top corners before and after moving/rotating; the spline should'
        Write-Host 'remain attached to the block. Pass through its old visual/collision position.'
    } else {
        Write-Host 'This run uses the shipped Blender Feature Park (the release default).'
        Write-Host 'It now contains 61 independent editor objects with owned collision ranges.'
        Write-Host 'Every collision-bearing editor object remains registered for the full run;'
        Write-Host 'moving one rebuilds only that object rather than the complete map.'
        Write-Host 'Confirm ordinary skating and park collision first. Press controller Y'
        Write-Host '(keyboard F only if using keyboard gameplay), put both feet'
        Write-Host 'on the normal concrete foundation, and walk for at least ten seconds.'
        Write-Host 'Enter G, confirm the skater stays visible, and hold RMB to look.'
        Write-Host 'Move and rotate LongManualPad, then G-exit and collide with it at its new'
        Write-Host 'pose while confirming its old location is empty. Repeat with StraightRail'
        Write-Host 'and grind it before and after moving; its spline must follow the rail.'
        Write-Host 'For spawning: enter G, aim at open ground, press E, select Test Grind Ledge,'
        Write-Host 'and click Spawn selected (or press Enter). It should appear where aimed, already'
        Write-Host 'selected. Move/rotate it, collide with it, then grind its blue top edge.'
        Write-Host 'Press E again to close the list without spawning.'
        Write-Host 'The hinged physics door remains simulation-controlled rather than editable.'
        Write-Host 'Do not use the clearly marked InstantBailHazard for the on-foot floor test.'
    }
    if ($UseGeneratedEditorMap) {
        Write-Host 'After G exit, press controller Y (keyboard F only for keyboard gameplay),'
        Write-Host 'let both feet touch the'
        Write-Host 'known-good park floor, then walk for at least ten seconds.'
    }
    Write-Host ''
    Write-Host 'Launching visible game window. Close it to finish telemetry.'

    $process = Start-Process -FilePath (
        Join-Path $runRoot 'skate3.exe'
    ) -WorkingDirectory $runRoot -ArgumentList $arguments -PassThru
    while (-not $process.HasExited) {
        $status = Read-EditorStatus
        if (-not [string]::IsNullOrWhiteSpace($status)) {
            '{0:o} {1}' -f (Get-Date), $status |
                Add-Content -LiteralPath $telemetryLog -Encoding UTF8
        }
        Start-Sleep -Seconds 2
        $process.Refresh()
    }
    if ($process.ExitCode -ne 0) {
        throw "Game exited with code $($process.ExitCode). See $eventLog"
    }
    Write-Host ''
    Write-Host "Visual check finished. Logs: $logRoot"
} catch {
    Write-Error $_
    exit 1
}
