[CmdletBinding()]
param(
    [ValidateRange(2, 5)]
    [int]$Clients = 3,
    [string]$SourceInstallRoot = '',
    [ValidateSet('release', 'relwithdebinfo')]
    [string]$BuildPreset = 'release',
    [string]$CacAssetRoot = '',
    [switch]$NoDirectBoot,
    [switch]$PrepareOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot '..')
)
$documentsRoot = Split-Path (
    Split-Path $repoRoot -Parent
) -Parent
$runParentName = if ($PrepareOnly) {
    'visual-check-preflight'
} else {
    'visual-checks'
}
$runParent = Join-Path $repoRoot "out\$runParentName"
$shortCommit = (& git -C $repoRoot rev-parse --short=8 HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($shortCommit)) {
    throw 'Unable to identify the dedicated multiplayer worktree commit.'
}
$timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$runRoot = Join-Path $runParent "$timestamp-$shortCommit"
if (Test-Path -LiteralPath $runRoot) {
    $runRoot = Join-Path $runParent "$timestamp-$shortCommit-$PID"
}
New-Item -ItemType Directory -Path $runRoot -Force | Out-Null
$setupLog = Join-Path $runRoot 'setup.log'

function Write-Setup {
    param([Parameter(Mandatory)][string]$Message)

    $line = '[{0}] {1}' -f (
        Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
    ), $Message
    $line | Add-Content -LiteralPath $setupLog -Encoding UTF8
    Write-Host $line
}

function Invoke-LoggedNative {
    param(
        [Parameter(Mandatory)][string]$Label,
        [Parameter(Mandatory)][string]$FilePath,
        [Parameter(Mandatory)][string[]]$Arguments,
        [switch]$Quiet
    )

    Write-Setup "$Label"
    $previousErrorPreference = $ErrorActionPreference
    try {
        # Windows PowerShell 5 wraps native stderr as ErrorRecord objects.
        # CMake writes ordinary warnings and status text there, so judge the
        # command by its native exit code instead of terminating on stderr.
        $ErrorActionPreference = 'Continue'
        & $FilePath @Arguments 2>&1 |
            ForEach-Object {
                $text = [string]$_
                $text | Add-Content -LiteralPath $setupLog -Encoding UTF8
                if (-not $Quiet) {
                    Write-Host $text
                }
            }
        $nativeExitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorPreference
    }
    if ($nativeExitCode -ne 0) {
        throw "$Label failed with exit code $nativeExitCode."
    }
}

function Test-PlayerRoot {
    param([Parameter(Mandatory)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        return $false
    }
    foreach ($relative in @('game', 'maps')) {
        if (-not (Test-Path -LiteralPath (
                    Join-Path $Path $relative
                ) -PathType Container)) {
            return $false
        }
    }
    return $true
}

function Resolve-PlayerRoot {
    param([string]$Requested)

    $candidates = New-Object System.Collections.Generic.List[string]
    if (-not [string]::IsNullOrWhiteSpace($Requested)) {
        $candidates.Add($Requested)
    }
    if (-not [string]::IsNullOrWhiteSpace(
            $env:SKATE3_PLAYER_ROOT
        )) {
        $candidates.Add($env:SKATE3_PLAYER_ROOT)
    }
    $candidates.Add(
        (Join-Path $documentsRoot (
            'Skate3Research\Skate3CustomEngineLayer-Player'
        ))
    )
    $candidates.Add(
        (Join-Path $documentsRoot 'Skate3CustomEngineLayer-Player')
    )

    foreach ($candidate in $candidates) {
        $fullPath = [System.IO.Path]::GetFullPath($candidate)
        if (Test-PlayerRoot $fullPath) {
            return $fullPath
        }
    }
    throw (
        'No complete player-data root was found. Pass -SourceInstallRoot ' +
        'or set SKATE3_PLAYER_ROOT to a directory containing game and maps.'
    )
}

function Test-CompleteCacAssetRoot {
    param([Parameter(Mandatory)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        return $false
    }
    $createACaracterRoot = Split-Path (
        Split-Path $Path -Parent
    ) -Parent
    $textureRoot = Join-Path $createACaracterRoot 'texture'
    if (-not (Test-Path -LiteralPath $textureRoot -PathType Container)) {
        return $false
    }
    $largeTexture = Get-ChildItem -LiteralPath $textureRoot `
        -File -Filter '*.rx2' |
        Where-Object Length -GT 131072 |
        Select-Object -First 1
    return $null -ne $largeTexture
}

function Resolve-CacAssetRoot {
    param(
        [string]$Requested,
        [Parameter(Mandatory)][string]$PlayerRoot
    )

    if (-not [string]::IsNullOrWhiteSpace($Requested)) {
        $fullPath = [System.IO.Path]::GetFullPath($Requested)
        if (-not (Test-CompleteCacAssetRoot $fullPath)) {
            throw "Incomplete Create-a-Skater asset root: $fullPath"
        }
        return $fullPath
    }

    $cacheRoot = Join-Path $repoRoot 'out\createacharacter-full'
    $candidate = Join-Path $cacheRoot (
        'data\content\createacharacter\model\cas_db'
    )
    if (Test-CompleteCacAssetRoot $candidate) {
        return $candidate
    }

    $archive = Join-Path $PlayerRoot (
        'game\data\content\createacharacter.big'
    )
    $extractor = Join-Path $documentsRoot (
        'Skate3Research\UTT-1.1.7\assets\bigfile.exe'
    )
    if (-not (Test-Path -LiteralPath $archive -PathType Leaf) -or
        -not (Test-Path -LiteralPath $extractor -PathType Leaf)) {
        throw (
            'The dedicated CAC cache is missing and its archive/extractor ' +
            'could not be found. Pass -CacAssetRoot explicitly.'
        )
    }

    Write-Setup (
        'Extracting the complete Create-a-Skater catalogue into the ' +
        'dedicated multiplayer worktree cache.'
    )
    New-Item -ItemType Directory -Path $cacheRoot -Force | Out-Null
    Push-Location $cacheRoot
    try {
        Invoke-LoggedNative -Label 'CAC catalogue extraction' `
            -FilePath $extractor -Arguments @($archive, '-x') -Quiet
    } finally {
        Pop-Location
    }
    if (-not (Test-CompleteCacAssetRoot $candidate)) {
        throw 'CAC extraction completed without a usable full asset root.'
    }
    return $candidate
}

function New-Junction {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Target
    )

    if (Test-Path -LiteralPath $Path) {
        throw "Refusing to replace existing staging path: $Path"
    }
    New-Item -ItemType Junction -Path $Path -Target $Target |
        Out-Null
}

function ConvertTo-BatchArgument {
    param([Parameter(Mandatory)][string]$Value)

    if ($Value -match '[\s"&|<>^]') {
        return '"' + ($Value -replace '"', '\"') + '"'
    }
    return $Value
}

$startedProcesses = New-Object System.Collections.Generic.List[object]
try {
    Write-Setup "Repository: $repoRoot"
    Write-Setup "Commit: $shortCommit"
    Write-Setup "Run directory: $runRoot"

    if (-not $PrepareOnly) {
        $running = Get-Process -Name 'skate3' `
            -ErrorAction SilentlyContinue
        if ($null -ne $running) {
            throw (
                'Close every existing skate3.exe process before starting ' +
                'an isolated visual-check run.'
            )
        }
    }

    $generatedInit = Join-Path $repoRoot 'generated\skate3_init.cpp'
    $generatedSources = Join-Path $repoRoot 'generated\sources.cmake'
    if (-not (Test-Path -LiteralPath $generatedInit -PathType Leaf) -or
        -not (Test-Path -LiteralPath $generatedSources -PathType Leaf)) {
        throw (
            'The dedicated worktree has no validated generated TU3 cache. ' +
            'An agent must seed its ignored generated directory before ' +
            'this visual check can build.'
        )
    }

    $playerRoot = Resolve-PlayerRoot $SourceInstallRoot
    $gameRoot = Join-Path $playerRoot 'game'
    $mapsRoot = Join-Path $playerRoot 'maps'
    $resolvedCacRoot = Resolve-CacAssetRoot `
        -Requested $CacAssetRoot -PlayerRoot $playerRoot
    Write-Setup "Read-only game data: $gameRoot"
    Write-Setup "Read-only maps: $mapsRoot"
    Write-Setup "Dedicated CAC cache: $resolvedCacRoot"

    $configureArguments = @(
        '--preset', $BuildPreset,
        "-DSKATE3_GAME_DATA_ROOT:PATH=$gameRoot",
        '-DSKATE3_MULTIPLAYER_BUILD_TESTS:BOOL=ON'
    )
    if ($env:OS -eq 'Windows_NT') {
        $clangC = 'C:\Program Files\LLVM\bin\clang.exe'
        $clangCxx = 'C:\Program Files\LLVM\bin\clang++.exe'
        $windowsKitsBin = (
            'C:\Program Files (x86)\Windows Kits\10\bin'
        )
        $rcCompiler = $null
        if (Test-Path -LiteralPath $windowsKitsBin -PathType Container) {
            $kitVersions = Get-ChildItem -LiteralPath $windowsKitsBin `
                -Directory |
                Where-Object Name -Match '^\d+\.\d+\.\d+\.\d+$' |
                Sort-Object {
                    [version]$_.Name
                } -Descending
            foreach ($kitVersion in $kitVersions) {
                $candidateRc = Join-Path $kitVersion.FullName 'x64\rc.exe'
                if (Test-Path -LiteralPath $candidateRc -PathType Leaf) {
                    $rcCompiler = $candidateRc
                    break
                }
            }
        }
        if (-not (Test-Path -LiteralPath $clangC -PathType Leaf) -or
            -not (Test-Path -LiteralPath $clangCxx -PathType Leaf) -or
            $null -eq $rcCompiler) {
            throw (
                'The supported Windows LLVM compiler or Windows SDK ' +
                'resource compiler was not found.'
            )
        }
        $configureArguments += @(
            "-DCMAKE_C_COMPILER:FILEPATH=$clangC",
            "-DCMAKE_CXX_COMPILER:FILEPATH=$clangCxx",
            "-DCMAKE_RC_COMPILER:FILEPATH=$rcCompiler"
        )
    }
    Invoke-LoggedNative -Label (
        "Configuring dedicated $BuildPreset build"
    ) -FilePath 'cmake' -Arguments $configureArguments
    Invoke-LoggedNative -Label (
        "Building dedicated $BuildPreset binaries"
    ) -FilePath 'cmake' -Arguments @(
        '--build', '--preset', $BuildPreset, '--parallel'
    )

    $buildRoot = Join-Path $repoRoot "out\build\$BuildPreset"
    Invoke-LoggedNative -Label (
        'Running multiplayer protocol and lifecycle tests'
    ) -FilePath 'ctest' -Arguments @(
        '--test-dir', $buildRoot, '--output-on-failure'
    )

    $builtExecutable = Join-Path $buildRoot 'skate3.exe'
    $builtRuntime = Join-Path $buildRoot 'rexruntime.dll'
    foreach ($required in @($builtExecutable, $builtRuntime)) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "Dedicated build output is missing: $required"
        }
    }

    $cache = Join-Path $buildRoot 'CMakeCache.txt'
    $expectedHome = (
        'CMAKE_HOME_DIRECTORY:INTERNAL=' +
        ($repoRoot -replace '\\', '/')
    )
    $cacheText = Get-Content -LiteralPath $cache -Raw
    if ($cacheText -notlike "*$expectedHome*") {
        throw (
            'The CMake cache does not belong to this multiplayer worktree: ' +
            $cache
        )
    }

    $executableHash = (
        Get-FileHash -LiteralPath $builtExecutable -Algorithm SHA256
    ).Hash
    $runtimeHash = (
        Get-FileHash -LiteralPath $builtRuntime -Algorithm SHA256
    ).Hash

    $gitStatus = & git -C $repoRoot status --short
    $gitStatus | Set-Content -LiteralPath (
        Join-Path $runRoot 'git-status.txt'
    ) -Encoding UTF8
    & git -C $repoRoot diff --binary | Set-Content -LiteralPath (
        Join-Path $runRoot 'worktree.patch'
    ) -Encoding UTF8
    & git -C $repoRoot submodule status --recursive |
        Set-Content -LiteralPath (
            Join-Path $runRoot 'submodules.txt'
        ) -Encoding UTF8

    $manifest = [ordered]@{
        schema = 1
        created_local = (Get-Date).ToString('o')
        prepare_only = [bool]$PrepareOnly
        repository = $repoRoot
        commit = (& git -C $repoRoot rev-parse HEAD).Trim()
        branch = (& git -C $repoRoot branch --show-current).Trim()
        dirty = @($gitStatus).Count -gt 0
        build_preset = $BuildPreset
        build_directory = $buildRoot
        executable_sha256 = $executableHash
        runtime_sha256 = $runtimeHash
        player_data_root = $playerRoot
        cac_asset_root = $resolvedCacRoot
        clients = $Clients
        transport = 'localhost-udp'
        quality_preset = 2
        root_rate_hz = 60
        animation_rate_hz = 60
        interpolation_ms = 50
        automated_tests = @(
            'skate3_multiplayer_protocol_tests',
            'skate3_multiplayer_lifecycle_tests'
        )
    }
    $manifest | ConvertTo-Json -Depth 4 |
        Set-Content -LiteralPath (
            Join-Path $runRoot 'run-manifest.json'
        ) -Encoding UTF8

    $instructions = @"
MULTIPLAYER VISUAL CHECK

Run directory:
$runRoot

Clients: $Clients
Transport: localhost UDP
Quality: Balanced, 60 Hz root, 60 Hz animation, 50 ms interpolation

Visual scenario:
1. Wait until every client has loaded the same map and all remote skaters appear.
2. On each client, inspect every other player: body, top, trousers, shoes,
   hair/hat, accessories, board deck, trucks, and wheels.
3. Skate on each client. Perform pushes, carving, ollies, flip tricks, grabs,
   spins, grinds, a bail, and a board-detached/board-return sequence.
4. Watch nearby remote players for pose fidelity, smooth motion, snapping,
   freezing, attachment drift, duplicated pieces, stale pieces, or flicker.
5. Confirm the focused local client keeps normal input response and has no
   obvious new frame stalls.
6. Close client 3 only. Wait at least 3 seconds for it to disappear from
   clients 1 and 2. Then run RELAUNCH_MULTIPLAYER_VISUAL_CLIENT_3.bat from
   the repository root. Confirm role 3 returns with its correct current
   appearance and animation, without the old remote state being reused.
7. Continue for at least 2 minutes after the reconnect, then close all clients.

Do not treat logs as proof of visual correctness. Report the visual result
separately, then ask the agent to analyze this run directory.
"@
    $instructions | Set-Content -LiteralPath (
        Join-Path $runRoot 'VISUAL-CHECK.txt'
    ) -Encoding UTF8

    $seedRoot = Join-Path $env:APPDATA 'skate3'
    $stagedClients = @()
    foreach ($role in 1..$Clients) {
        $clientRoot = Join-Path $runRoot "clients\client$role"
        New-Item -ItemType Directory -Path $clientRoot -Force |
            Out-Null
        Copy-Item -LiteralPath $builtExecutable -Destination (
            Join-Path $clientRoot 'skate3.exe'
        )
        Copy-Item -LiteralPath $builtRuntime -Destination (
            Join-Path $clientRoot 'rexruntime.dll'
        )
        New-Item -ItemType File -Path (
            Join-Path $clientRoot 'portable.txt'
        ) -Force | Out-Null
        New-Junction -Path (Join-Path $clientRoot 'game') `
            -Target $gameRoot
        New-Junction -Path (Join-Path $clientRoot 'maps') `
            -Target $mapsRoot

        if (Test-Path -LiteralPath $seedRoot -PathType Container) {
            Get-ChildItem -LiteralPath $seedRoot -Directory |
                Where-Object Name -Match '^[0-9A-Fa-f]{16}$' |
                ForEach-Object {
                    Copy-Item -LiteralPath $_.FullName -Destination (
                        Join-Path $clientRoot $_.Name
                    ) -Recurse
                }
        }

        $arguments = @(
            '--fullscreen=false',
            '--window_width=1120',
            '--window_height=630',
            '--draw_resolution_scale_x=1',
            '--draw_resolution_scale_y=1',
            '--skate3_input_lab=false',
            '--skate3_multiplayer_local_visuals=true',
            '--skate3_multiplayer_local_lane_spacing=0',
            "--skate3_multiplayer_local_client=$role",
            '--skate3_multiplayer_quality_preset=2',
            '--skate3_multiplayer_local_send_rate=60',
            '--skate3_multiplayer_local_animation_rate=60',
            '--skate3_multiplayer_local_interpolation_ms=50',
            '--skate3_native_render_scene_perf_log=true',
            '--skate3_native_render_scene_perf_interval=300'
        )
        if (-not $NoDirectBoot) {
            $arguments += '--skate3_direct_boot=true'
        }
        $arguments += (
            '--skate3_multiplayer_cac_asset_root={0}' -f
            $resolvedCacRoot
        )

        $argumentFile = Join-Path $clientRoot 'launch-arguments.txt'
        $arguments | Set-Content -LiteralPath $argumentFile `
            -Encoding UTF8
        $relaunchCommand = @(
            '@echo off'
            'cd /d "{0}"' -f $clientRoot
            'start "" "{0}" {1}' -f (
                Join-Path $clientRoot 'skate3.exe'
            ), (($arguments | ForEach-Object {
                ConvertTo-BatchArgument $_
            }) -join ' ')
        )
        $relaunchCommand | Set-Content -LiteralPath (
            Join-Path $clientRoot "relaunch-client$role.bat"
        ) -Encoding ASCII

        $stagedClients += [pscustomobject]@{
            Role = $role
            Root = $clientRoot
            Executable = Join-Path $clientRoot 'skate3.exe'
            Arguments = $arguments
        }
    }

    if ($PrepareOnly) {
        Write-Setup (
            'PrepareOnly completed: build and staging passed; no game ' +
            'client was launched.'
        )
        Write-Host ''
        Write-Host 'NON-GAME PREFLIGHT PASSED'
        Write-Host "Artifacts: $runRoot"
        exit 0
    }

    foreach ($client in $stagedClients) {
        Write-Setup (
            'Launching user visual-check client {0} from {1}' -f
            $client.Role, $client.Root
        )
        $process = Start-Process -FilePath $client.Executable `
            -WorkingDirectory $client.Root `
            -ArgumentList $client.Arguments -PassThru
        $startedProcesses.Add($process)
    }
    $startedProcesses | ForEach-Object {
        '{0} {1}' -f $_.Id, $_.Path
    } | Set-Content -LiteralPath (
        Join-Path $runRoot 'launched-processes.txt'
    ) -Encoding UTF8

    New-Item -ItemType Directory -Path $runParent -Force | Out-Null
    $runRoot | Set-Content -LiteralPath (
        Join-Path $runParent 'LATEST.txt'
    ) -Encoding UTF8

    Write-Host ''
    Write-Host 'MULTIPLAYER VISUAL CHECK READY'
    Write-Host "Run folder: $runRoot"
    Write-Host "Instructions: $(Join-Path $runRoot 'VISUAL-CHECK.txt')"
    Write-Host ''
    Write-Host (
        'After closing client 3, wait 3 seconds and run ' +
        'RELAUNCH_MULTIPLAYER_VISUAL_CLIENT_3.bat.'
    )
    Write-Host (
        'After the full check, close every client and tell the agent ' +
        'that the run is complete.'
    )
} catch {
    $message = $_.Exception.Message
    $message | Set-Content -LiteralPath (
        Join-Path $runRoot 'setup-error.txt'
    ) -Encoding UTF8
    Write-Setup "ERROR: $message"

    foreach ($process in $startedProcesses) {
        if (-not $process.HasExited) {
            Stop-Process -Id $process.Id -ErrorAction SilentlyContinue
        }
    }
    Write-Error (
        "$message`nVisual-check artifacts: $runRoot"
    )
    exit 1
}
