[CmdletBinding()]
param(
    [string]$SourceInstallRoot = '',
    [switch]$PrepareOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot '..')
)
$shortCommit = (& git -C $repoRoot rev-parse --short=8 HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or
    [string]::IsNullOrWhiteSpace($shortCommit)) {
    throw 'Unable to identify the multiplayer worktree commit.'
}
$timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$runParent = Join-Path $repoRoot 'out\visual-checks'
$runRoot = Join-Path (
    $runParent
) "$timestamp-$shortCommit-mixed-version"
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
        [Parameter(Mandatory)][string[]]$Arguments
    )

    Write-Setup $Label
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $FilePath @Arguments 2>&1 | ForEach-Object {
            $text = [string]$_
            $text | Add-Content -LiteralPath $setupLog -Encoding UTF8
            Write-Host $text
        }
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $oldPreference
    }
    if ($exitCode -ne 0) {
        throw "$Label failed with exit code $exitCode."
    }
}

function Resolve-InstallRoot {
    param([string]$Requested)

    $candidates = New-Object System.Collections.Generic.List[string]
    if (-not [string]::IsNullOrWhiteSpace($Requested)) {
        $candidates.Add($Requested)
    }
    $candidates.Add(
        'C:\Users\Daddy\Documents\SK8 Engine - Latest Release'
    )
    if (-not [string]::IsNullOrWhiteSpace(
            $env:SKATE3_PLAYER_ROOT
        )) {
        $candidates.Add($env:SKATE3_PLAYER_ROOT)
    }
    foreach ($candidate in $candidates) {
        $absolute = [System.IO.Path]::GetFullPath($candidate)
        if ((Test-Path -LiteralPath (
                    Join-Path $absolute 'game\default.xex'
                ) -PathType Leaf) -and
            (Test-Path -LiteralPath (
                    Join-Path $absolute 'maps'
                ) -PathType Container)) {
            return $absolute
        }
    }
    throw (
        'A complete SK8 Engine install was not found. Pass ' +
        '-SourceInstallRoot with a folder containing game and maps.'
    )
}

function New-Junction {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Target
    )

    New-Item -ItemType Junction -Path $Path -Target $Target |
        Out-Null
}

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
                'the isolated mixed-version check.'
            )
        }
    }

    $installRoot = Resolve-InstallRoot $SourceInstallRoot
    $gameRoot = Join-Path $installRoot 'game'
    $mapsRoot = Join-Path $installRoot 'maps'
    Write-Setup "Read-only game data: $gameRoot"
    Write-Setup "Read-only maps: $mapsRoot"
    Write-Setup (
        'CAC source: automatic createacharacter.big discovery; no ' +
        'skate3_multiplayer_cac_asset_root override'
    )

    $buildRoot = Join-Path $repoRoot 'out\build\release'
    if (-not (Test-Path -LiteralPath (
                Join-Path $buildRoot 'CMakeCache.txt'
            ) -PathType Leaf)) {
        Invoke-LoggedNative -Label 'Configuring Release build' `
            -FilePath 'cmake' -Arguments @(
                '--preset', 'release',
                "-DSKATE3_GAME_DATA_ROOT:PATH=$gameRoot",
                '-DSKATE3_MULTIPLAYER_BUILD_TESTS:BOOL=ON'
            )
    }
    Invoke-LoggedNative -Label (
        'Incrementally building the updated client'
    ) -FilePath 'cmake' -Arguments @(
        '--build', '--preset', 'release', '--parallel'
    )
    Invoke-LoggedNative -Label (
        'Running non-game automated tests'
    ) -FilePath 'ctest' -Arguments @(
        '--test-dir', $buildRoot, '--output-on-failure'
    )

    $builtExecutable = Join-Path $buildRoot 'skate3.exe'
    $builtRuntime = Join-Path $buildRoot 'rexruntime.dll'
    foreach ($required in @($builtExecutable, $builtRuntime)) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "Build output is missing: $required"
        }
    }

    $clientRoot = Join-Path $runRoot 'clients\client1'
    New-Item -ItemType Directory -Path $clientRoot -Force |
        Out-Null
    New-Item -ItemType Directory -Path (
        Join-Path $clientRoot 'logs'
    ) -Force | Out-Null
    Copy-Item -LiteralPath $builtExecutable -Destination (
        Join-Path $clientRoot 'skate3.exe'
    )
    Copy-Item -LiteralPath $builtRuntime -Destination (
        Join-Path $clientRoot 'rexruntime.dll'
    )
    foreach ($steamFile in @('steam_api64.dll', 'steam_appid.txt')) {
        $source = Join-Path $installRoot $steamFile
        if (Test-Path -LiteralPath $source -PathType Leaf) {
            Copy-Item -LiteralPath $source -Destination (
                Join-Path $clientRoot $steamFile
            )
        }
    }
    $steamCache = Join-Path $installRoot '.cel-steam'
    if (Test-Path -LiteralPath $steamCache -PathType Container) {
        Copy-Item -LiteralPath $steamCache -Destination $clientRoot `
            -Recurse
    }
    New-Junction -Path (Join-Path $clientRoot 'game') `
        -Target $gameRoot
    New-Junction -Path (Join-Path $clientRoot 'maps') `
        -Target $mapsRoot

    $arguments = @(
        '--skate3_multiplayer_local_visuals=true',
        '--skate3_multiplayer_replication_worker=true',
        '--skate3_multiplayer_async_appearance_prepare=true',
        '--skate3_multiplayer_incremental_appearance_install=true',
        '--skate3_multiplayer_appearance_install_ops_per_frame=4',
        '--skate3_multiplayer_appearance_install_budget_ms=4',
        '--skate3_native_render_scene_perf_log=true',
        '--skate3_native_render_scene_perf_interval=300'
    )
    $arguments | Set-Content -LiteralPath (
        Join-Path $clientRoot 'launch-arguments.txt'
    ) -Encoding UTF8

    $gitStatus = @(& git -C $repoRoot status --short)
    $gitStatus | Set-Content -LiteralPath (
        Join-Path $runRoot 'git-status.txt'
    ) -Encoding UTF8
    & git -C $repoRoot diff --binary | Set-Content -LiteralPath (
        Join-Path $runRoot 'worktree.patch'
    ) -Encoding UTF8
    $manifest = [ordered]@{
        schema = 1
        created_local = (Get-Date).ToString('o')
        repository = $repoRoot
        commit = (& git -C $repoRoot rev-parse HEAD).Trim()
        dirty = $gitStatus.Count -gt 0
        source_install = $installRoot
        client_root = $clientRoot
        transport = 'Steam P2P selected in game UI'
        local_version = 'automatic-CAC hotfix'
        peer_version = 'v0.1.0-preview.12 supported'
        wire_protocol = 12
        explicit_cac_root = $false
        prepare_only = [bool]$PrepareOnly
    }
    $manifest | ConvertTo-Json -Depth 4 |
        Set-Content -LiteralPath (
            Join-Path $runRoot 'manifest.json'
        ) -Encoding UTF8

    @"
MIXED-VERSION APPEARANCE CHECK

This launches one updated local client. The other player may remain on
v0.1.0-preview.12 because the wire protocol is still v12.

1. Use the normal Multiplayer menu to join each other over Steam.
2. Stay near one another for 5 minutes and skate normally.
3. The updated local client should replace the teal proxy quickly and show
   the old-version peer's complete outfit, hair, shirt, board, and animation.
4. Change the old-version peer's outfit once if convenient, then return to
   the session and watch it settle.

Success:
- The updated client logs "automatic CAC catalogue ready".
- The old peer becomes a complete skater without a roughly 30-second
  multi-megabyte legacy appearance wait.
- No shirt detaches, disappears, stretches, or trails toward the marker.
- No repeated "CAC catalogue is unavailable" warning flood.
- Normal skating remains smooth after the appearance arrives.

Mixed-version limitation:
- This test proves the updated receiver can render an old peer.
- The old peer does not have this hotfix, so their view of the updated player
  can still show the old slow/fallback behavior. That does not disprove the
  receiver-side fix.

Visual correctness is your decision. Logs are analyzed separately afterward.
"@ | Set-Content -LiteralPath (
        Join-Path $runRoot 'VISUAL-CHECK.txt'
    ) -Encoding UTF8

    New-Item -ItemType Directory -Path $runParent -Force |
        Out-Null
    $runRoot | Set-Content -LiteralPath (
        Join-Path $runParent 'LATEST.txt'
    ) -Encoding UTF8

    if ($PrepareOnly) {
        Write-Setup (
            'PrepareOnly completed; build, tests, and staging passed. ' +
            'No game client was launched.'
        )
        Write-Host ''
        Write-Host 'MIXED-VERSION NON-GAME PREFLIGHT PASSED'
        Write-Host "Artifacts: $runRoot"
        exit 0
    }

    Write-Setup 'Launching the updated client for the user-run check'
    $process = Start-Process -FilePath (
        Join-Path $clientRoot 'skate3.exe'
    ) -WorkingDirectory $clientRoot -ArgumentList $arguments `
        -PassThru
    "$($process.Id) $($process.Path)" | Set-Content -LiteralPath (
        Join-Path $runRoot 'launched-processes.txt'
    ) -Encoding UTF8

    Write-Host ''
    Write-Host 'MIXED-VERSION APPEARANCE CHECK READY'
    Write-Host "Run folder: $runRoot"
    Write-Host "Instructions: $(Join-Path $runRoot 'VISUAL-CHECK.txt')"
} catch {
    $_ | Out-String | Set-Content -LiteralPath (
        Join-Path $runRoot 'setup-error.txt'
    ) -Encoding UTF8
    Write-Error $_
    Write-Host "Visual-check artifacts: $runRoot"
    exit 1
}
