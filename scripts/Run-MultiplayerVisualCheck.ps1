[CmdletBinding()]
param(
    [ValidateRange(2, 5)]
    [int]$Clients = 3,
    [string]$SourceInstallRoot = '',
    [ValidateSet('release', 'relwithdebinfo')]
    [string]$BuildPreset = 'release',
    [string]$CacAssetRoot = '',
    [switch]$NoDirectBoot,
    [switch]$PrepareOnly,
    [switch]$AppearanceRecoveryCheck
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
if ($AppearanceRecoveryCheck -and $Clients -lt 3) {
    throw 'The appearance-recovery check requires three clients.'
}

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

function Get-ProfileDirectories {
    param([Parameter(Mandatory)][string]$Root)

    if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
        return @()
    }
    return @(
        Get-ChildItem -LiteralPath $Root -Directory -Force |
            Where-Object Name -Match '^[0-9A-Fa-f]{16}$'
    )
}

function Initialize-RoleProfileStore {
    param(
        [Parameter(Mandatory)][int]$Role,
        [Parameter(Mandatory)][string]$Store,
        [Parameter(Mandatory)][string]$AppDataSeed
    )

    New-Item -ItemType Directory -Path $Store -Force |
        Out-Null
    if (@(Get-ProfileDirectories $Store).Count -gt 0) {
        Write-Setup (
            "Using persistent profile storage for role ${Role}: $Store"
        )
        return
    }

    $sources = New-Object System.Collections.Generic.List[string]
    $latestPointer = Join-Path (
        Join-Path $repoRoot 'out\visual-checks'
    ) 'LATEST.txt'
    if (Test-Path -LiteralPath $latestPointer -PathType Leaf) {
        $latestRun = (
            Get-Content -LiteralPath $latestPointer -Raw
        ).Trim()
        if (-not [string]::IsNullOrWhiteSpace($latestRun)) {
            $sources.Add(
                (Join-Path $latestRun "clients\client$Role")
            )
        }
    }
    $sources.Add($AppDataSeed)

    foreach ($source in $sources) {
        $profiles = @(Get-ProfileDirectories $source)
        if ($profiles.Count -eq 0) {
            continue
        }
        foreach ($profile in $profiles) {
            Copy-Item -LiteralPath $profile.FullName -Destination (
                Join-Path $Store $profile.Name
            ) -Recurse
        }
        Write-Setup (
            "Seeded persistent role ${Role} profile from $source"
        )
        return
    }
    throw (
        "No portable profile seed was found for role $Role. " +
        "Expected a 16-hex profile under the latest visual run or " +
        "$AppDataSeed."
    )
}

function ConvertTo-BatchArgument {
    param([Parameter(Mandatory)][string]$Value)

    if ($Value -match '[\s"&|<>^]') {
        return '"' + ($Value -replace '"', '\"') + '"'
    }
    return $Value
}

function Get-CMakeCacheValue {
    param(
        [Parameter(Mandatory)][string]$CacheText,
        [Parameter(Mandatory)][string]$Name
    )

    $match = [regex]::Match(
        $CacheText,
        '(?m)^' + [regex]::Escape($Name) + ':[^=]+=(.*)$'
    )
    if (-not $match.Success) {
        return $null
    }
    return $match.Groups[1].Value.Trim()
}

function Test-SamePath {
    param(
        [AllowNull()][string]$Left,
        [Parameter(Mandatory)][string]$Right
    )

    if ([string]::IsNullOrWhiteSpace($Left)) {
        return $false
    }
    try {
        $normalizedLeft = $Left.Replace('/', '\')
        $leftFull = [System.IO.Path]::GetFullPath(
            $normalizedLeft
        )
        $rightFull = [System.IO.Path]::GetFullPath($Right)
        return [string]::Equals(
            $leftFull.TrimEnd('\'),
            $rightFull.TrimEnd('\'),
            [System.StringComparison]::OrdinalIgnoreCase
        )
    } catch {
        return $false
    }
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

    $buildRoot = Join-Path $repoRoot "out\build\$BuildPreset"
    $cache = Join-Path $buildRoot 'CMakeCache.txt'
    $buildGraph = Join-Path $buildRoot 'build.ninja'
    $expectedBuildType = if ($BuildPreset -eq 'release') {
        'Release'
    } else {
        'RelWithDebInfo'
    }
    $configureArguments = @(
        '--preset', $BuildPreset,
        "-DSKATE3_GAME_DATA_ROOT:PATH=$gameRoot",
        '-DSKATE3_MULTIPLAYER_BUILD_TESTS:BOOL=ON'
    )
    $clangC = $null
    $clangCxx = $null
    $rcCompiler = $null
    if ($env:OS -eq 'Windows_NT') {
        $clangC = 'C:\Program Files\LLVM\bin\clang.exe'
        $clangCxx = 'C:\Program Files\LLVM\bin\clang++.exe'
        $windowsKitsBin = (
            'C:\Program Files (x86)\Windows Kits\10\bin'
        )
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

    $compatibleCache = $false
    if ((Test-Path -LiteralPath $cache -PathType Leaf) -and
        (Test-Path -LiteralPath $buildGraph -PathType Leaf)) {
        $existingCache = Get-Content -LiteralPath $cache -Raw
        $compatibleCache = (
            (Get-CMakeCacheValue $existingCache 'CMAKE_GENERATOR') -eq
                'Ninja' -and
            (Get-CMakeCacheValue $existingCache 'CMAKE_BUILD_TYPE') -eq
                $expectedBuildType -and
            (Get-CMakeCacheValue (
                $existingCache
            ) 'SKATE3_MULTIPLAYER_BUILD_TESTS') -eq 'ON' -and
            (Test-SamePath (
                Get-CMakeCacheValue $existingCache 'CMAKE_HOME_DIRECTORY'
            ) $repoRoot) -and
            (Test-SamePath (
                Get-CMakeCacheValue $existingCache 'SKATE3_GAME_DATA_ROOT'
            ) $gameRoot)
        )
        if ($compatibleCache -and $env:OS -eq 'Windows_NT') {
            $compatibleCache = (
                (Test-SamePath (
                    Get-CMakeCacheValue $existingCache 'CMAKE_C_COMPILER'
                ) $clangC) -and
                (Test-SamePath (
                    Get-CMakeCacheValue $existingCache 'CMAKE_CXX_COMPILER'
                ) $clangCxx)
            )
        }
    }
    if ($compatibleCache) {
        Write-Setup (
            "Reusing compatible $BuildPreset CMake cache; Ninja will " +
            'reconfigure automatically only if build inputs require it.'
        )
    } else {
        Invoke-LoggedNative -Label (
            "Configuring dedicated $BuildPreset build"
        ) -FilePath 'cmake' -Arguments $configureArguments
    }
    Invoke-LoggedNative -Label (
        "Building dedicated $BuildPreset binaries"
    ) -FilePath 'cmake' -Arguments @(
        '--build', '--preset', $BuildPreset, '--parallel'
    )

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
        persistent_profile_root = (
            Join-Path $repoRoot 'out\visual-check-profiles'
        )
        clients = $Clients
        transport = 'localhost-udp'
        quality_preset = 2
        root_rate_hz = 60
        animation_rate_hz = 60
        interpolation_ms = 50
        appearance_recovery_check = [bool]$AppearanceRecoveryCheck
        appearance_recovery_receiver = if ($AppearanceRecoveryCheck) {
            3
        } else {
            0
        }
        appearance_recovery_sender = if ($AppearanceRecoveryCheck) {
            2
        } else {
            0
        }
        automated_tests = @(
            'skate3_multiplayer_protocol_tests',
            'skate3_multiplayer_lifecycle_tests'
        )
    }
    $manifest | ConvertTo-Json -Depth 4 |
        Set-Content -LiteralPath (
            Join-Path $runRoot 'run-manifest.json'
        ) -Encoding UTF8

    $instructions = if ($AppearanceRecoveryCheck) {
        @"
MULTIPLAYER APPEARANCE RECOVERY CHECK

Run directory:
$runRoot

Clients: $Clients
Transport: localhost UDP
Quality: Balanced, 60 Hz root, 60 Hz animation, 50 ms interpolation
Fault: client 3 intentionally drops role 2's final appearance chunk until
the bounded assembly timeout requests a resend.

Visual scenario:
1. Wait until every client has loaded the same map.
2. On client 1, confirm role 2 reaches its normal complete outfit promptly.
3. Watch role 2 from client 3. Role 2 should deliberately remain the teal
   proxy for roughly 10 seconds while one appearance chunk is withheld.
4. Without changing outfits or reconnecting, confirm role 2 automatically
   changes from the teal proxy to its exact complete outfit within 5 seconds
   after that timeout.
5. Skate as role 2 for another minute. On client 3, confirm the recovered
   outfit, board, attachments, and animation remain stable with no flicker,
   mixed pieces, or return to teal.
6. Confirm all focused local clients retain normal input response and show no
   obvious new frame stalls. Then close all clients.

Success:
- Only client 3 temporarily shows role 2 as teal during the intentional fault.
- Client 3 recovers role 2 automatically about 10-15 seconds after map load.
- The recovered appearance is complete and remains stable.

Failure:
- Client 3 never recovers role 2, recovers the wrong outfit, mixes old/new
  pieces, or returns to teal after recovery.
- Any client freezes, stalls, or loses normal local input response.

Do not treat logs as proof of visual correctness. Report the visual result
separately, then ask the agent to analyze this run directory.
"@
    } else {
        @"
MULTIPLAYER VISUAL CHECK

Run directory:
$runRoot

Clients: $Clients
Transport: localhost UDP
Quality: Balanced, 60 Hz root, 60 Hz animation, 50 ms interpolation
Profiles: persistent per role across visual-check runs

Visual scenario:
1. Wait until every client has loaded the same map and all remote skaters appear.
2. On each client, inspect every other player: body, top, trousers, shoes,
   hair/hat, accessories, board deck, trucks, and wheels.
3. On client 2, enter the wardrobe, change at least one obvious body item
   (top, trousers, shoes, or hat/hair) and one board item, save/apply the
   outfit, and return to skating. On clients 1 and 3, confirm role 2 changes
   to that exact outfit without becoming the teal proxy. Repeat on client 3
   while watching it from clients 1 and 2.
4. Skate on each client. Perform pushes, carving, ollies, flip tricks, grabs,
   spins, grinds, a bail, and a board-detached/board-return sequence.
5. Watch nearby remote players for pose fidelity, smooth motion, snapping,
   freezing, attachment drift, duplicated pieces, stale pieces, or flicker.
6. Confirm the focused local client keeps normal input response and has no
   obvious new frame stalls.
7. Close client 3 only. Wait at least 3 seconds for it to disappear from
   clients 1 and 2. Then run RELAUNCH_MULTIPLAYER_VISUAL_CLIENT_3.bat from
   the repository root. Confirm role 3 returns with its correct current
   appearance and animation, without the old remote state being reused.
8. Continue for at least 2 minutes after the reconnect, then close all clients.

Outfit/profile behavior:
- Each numbered client keeps its own writable profile under
  out\visual-check-profiles\clientN.
- Wardrobe changes made and saved on a role survive the next visual-check
  launcher run. Timestamped run folders and logs remain isolated.

Do not treat logs as proof of visual correctness. Report the visual result
separately, then ask the agent to analyze this run directory.
"@
    }
    $instructions | Set-Content -LiteralPath (
        Join-Path $runRoot 'VISUAL-CHECK.txt'
    ) -Encoding UTF8

    $seedRoot = Join-Path $env:APPDATA 'skate3'
    $persistentProfileRoot = Join-Path (
        Join-Path $repoRoot 'out'
    ) 'visual-check-profiles'
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

        $roleProfileStore = Join-Path (
            $persistentProfileRoot
        ) "client$role"
        Initialize-RoleProfileStore -Role $role `
            -Store $roleProfileStore -AppDataSeed $seedRoot
        foreach ($profile in @(
                Get-ProfileDirectories $roleProfileStore
            )) {
            New-Junction -Path (
                Join-Path $clientRoot $profile.Name
            ) -Target $profile.FullName
        }
        $profileRecipe = Get-ChildItem `
            -LiteralPath $roleProfileStore -Recurse -File `
            -Filter 'SKATER.P' |
            Sort-Object LastWriteTime -Descending |
            Select-Object -First 1
        if ($null -eq $profileRecipe) {
            throw (
                "Persistent role $role profile has no SKATER.P recipe: " +
                $roleProfileStore
            )
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
            '--skate3_native_render_scene_perf_interval=300',
            (
                '--skate3_multiplayer_local_profile_recipe={0}' -f
                $profileRecipe.FullName
            )
        )
        if (-not $NoDirectBoot) {
            $arguments += '--skate3_direct_boot=true'
        }
        if ($AppearanceRecoveryCheck -and $role -eq 3) {
            $arguments += (
                '--skate3_multiplayer_test_drop_appearance_role=2'
            )
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
    if ($AppearanceRecoveryCheck) {
        Write-Host 'MULTIPLAYER APPEARANCE RECOVERY CHECK READY'
    } else {
        Write-Host 'MULTIPLAYER VISUAL CHECK READY'
    }
    Write-Host "Run folder: $runRoot"
    Write-Host "Instructions: $(Join-Path $runRoot 'VISUAL-CHECK.txt')"
    Write-Host ''
    if (-not $AppearanceRecoveryCheck) {
        Write-Host (
            'After closing client 3, wait 3 seconds and run ' +
            'RELAUNCH_MULTIPLAYER_VISUAL_CLIENT_3.bat.'
        )
    }
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
