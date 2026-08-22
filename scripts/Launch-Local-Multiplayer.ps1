[CmdletBinding()]
param(
    [string]$SourceInstallRoot = '',
    [string]$BuildDirectory = '..\build\skate3-custom-engine-layer-release',
    [ValidateRange(2, 100)]
    [int]$Clients = 2,
    [string]$CacAssetRoot = '',
    [switch]$NoDirectBoot
)

$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot '..')
)
if ([string]::IsNullOrWhiteSpace($SourceInstallRoot)) {
    $SourceInstallRoot = Join-Path (
        Split-Path $repoRoot -Parent
    ) 'Skate3CustomEngineLayer-Player'
}
$SourceInstallRoot = [System.IO.Path]::GetFullPath($SourceInstallRoot)
$buildRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $repoRoot $BuildDirectory)
)
$clientRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $repoRoot 'out\local-multiplayer')
)

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

    # The old memory-snapshot catalogue padded every file to 128 KiB and
    # omitted the lower half of 512x512 clothing atlases. A complete archive
    # extraction contains many larger RX2 files.
    $largeTexture = Get-ChildItem -LiteralPath $textureRoot `
        -File -Filter '*.rx2' |
        Where-Object Length -GT 131072 |
        Select-Object -First 1
    return $null -ne $largeTexture
}

if ([string]::IsNullOrWhiteSpace($CacAssetRoot)) {
    $documentsRoot = Split-Path (
        Split-Path $repoRoot -Parent
    ) -Parent
    $cacheRoot = Join-Path $repoRoot 'out\createacharacter-full'
    $candidate = Join-Path $cacheRoot (
        'data\content\' +
        'createacharacter\model\cas_db'
    )
    if (-not (Test-CompleteCacAssetRoot $candidate)) {
        $archive = Join-Path $SourceInstallRoot (
            'game\data\content\createacharacter.big'
        )
        $extractor = Join-Path $documentsRoot (
            'Skate3Research\UTT-1.1.7\assets\bigfile.exe'
        )
        if ((Test-Path -LiteralPath $archive -PathType Leaf) -and
            (Test-Path -LiteralPath $extractor -PathType Leaf)) {
            Write-Host (
                'Extracting the complete Create-a-Skater catalogue once...'
            )
            New-Item -ItemType Directory -Path $cacheRoot -Force |
                Out-Null
            Push-Location $cacheRoot
            try {
                & $extractor $archive -x 2>&1 | Out-Null
                if ($LASTEXITCODE -ne 0) {
                    throw (
                        "bigfile.exe failed with exit code $LASTEXITCODE"
                    )
                }
            } finally {
                Pop-Location
            }
        }
    }
    if (Test-CompleteCacAssetRoot $candidate) {
        $CacAssetRoot = $candidate
    }
}
if (-not [string]::IsNullOrWhiteSpace($CacAssetRoot)) {
    $CacAssetRoot = [System.IO.Path]::GetFullPath($CacAssetRoot)
    if (-not (Test-CompleteCacAssetRoot $CacAssetRoot)) {
        throw (
            'Create-a-Skater asset root is missing complete RX2 texture ' +
            "payloads: $CacAssetRoot"
        )
    }
}

$builtExecutable = Join-Path $buildRoot 'skate3.exe'
$builtRuntime = Join-Path $buildRoot 'rexruntime.dll'
$sourceExecutable = Join-Path $SourceInstallRoot 'skate3.exe'
$sourceRuntime = Join-Path $SourceInstallRoot 'rexruntime.dll'
$executable = if (Test-Path -LiteralPath $builtExecutable -PathType Leaf) {
    $builtExecutable
} else {
    $sourceExecutable
}
$runtime = if (Test-Path -LiteralPath $builtRuntime -PathType Leaf) {
    $builtRuntime
} else {
    $sourceRuntime
}
$gameRoot = Join-Path $SourceInstallRoot 'game'
$mapsRoot = Join-Path $SourceInstallRoot 'maps'

foreach ($required in @($executable, $runtime, $gameRoot, $mapsRoot)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Local multiplayer source is missing: $required"
    }
}

$running = Get-Process -Name 'skate3' -ErrorAction SilentlyContinue
if ($null -ne $running) {
    throw 'Close existing skate3.exe processes before staging two clients.'
}

function Ensure-Junction {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Target
    )
    if (Test-Path -LiteralPath $Path) {
        $item = Get-Item -LiteralPath $Path -Force
        if (-not ($item.Attributes -band
                  [System.IO.FileAttributes]::ReparsePoint)) {
            throw "Expected a junction but found a real item: $Path"
        }
        return
    }
    New-Item -ItemType Junction -Path $Path -Target $Target | Out-Null
}

New-Item -ItemType Directory -Path $clientRoot -Force | Out-Null
$seedRoot = Join-Path $env:APPDATA 'skate3'
$stagedClients = @()
foreach ($role in 1..$Clients) {
    $root = Join-Path $clientRoot "client$role"
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    Copy-Item -LiteralPath $executable -Destination (
        Join-Path $root 'skate3.exe'
    ) -Force
    Copy-Item -LiteralPath $runtime -Destination (
        Join-Path $root 'rexruntime.dll'
    ) -Force
    New-Item -ItemType File -Path (
        Join-Path $root 'portable.txt'
    ) -Force | Out-Null
    Ensure-Junction -Path (Join-Path $root 'game') -Target $gameRoot
    Ensure-Junction -Path (Join-Path $root 'maps') -Target $mapsRoot

    # Seed each isolated portable client with the user's existing profile once.
    # The two copies can then save concurrently without sharing writable data.
    if (Test-Path -LiteralPath $seedRoot -PathType Container) {
        Get-ChildItem -LiteralPath $seedRoot -Directory |
            Where-Object Name -Match '^[0-9A-Fa-f]{16}$' |
            ForEach-Object {
                $destination = Join-Path $root $_.Name
                if (-not (Test-Path -LiteralPath $destination)) {
                    Copy-Item -LiteralPath $_.FullName -Destination $destination `
                        -Recurse
                }
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
        "--skate3_multiplayer_local_client=$role"
    )
    if (-not $NoDirectBoot) {
        $arguments += '--skate3_direct_boot=true'
    }
    if (-not [string]::IsNullOrWhiteSpace($CacAssetRoot)) {
        $arguments += (
            '--skate3_multiplayer_cac_asset_root={0}' -f
            $CacAssetRoot
        )
    }
    $stagedClients += [pscustomobject]@{
        Role = $role
        Root = $root
        Executable = Join-Path $root 'skate3.exe'
        Arguments = $arguments
    }
}

foreach ($client in $stagedClients) {
    Write-Host (
        "Launching local multiplayer client {0} from {1}" -f
        $client.Role, $client.Root
    )
    Start-Process -FilePath $client.Executable `
        -WorkingDirectory $client.Root `
        -ArgumentList $client.Arguments
}

Write-Host ''
Write-Host "$Clients clients use the same game and maps read-only through junctions."
Write-Host 'Their settings, caches, logs, and saves are isolated under:'
Write-Host "  $clientRoot"
Write-Host 'Client 1 is the logical host; nearby peers use the real animated skater and board.'
if (-not [string]::IsNullOrWhiteSpace($CacAssetRoot)) {
    Write-Host "CAC bind assets: $CacAssetRoot"
}
Write-Host 'Remote collision is disabled.'
