[CmdletBinding()]
param(
    [switch]$RetailCollisionOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot '..\..\..')
)
$preparedRoot = Join-Path $repoRoot 'out\university-visual-check\prepared'
$manifestPath = Join-Path $preparedRoot 'prepared-manifest.json'
$runTimestamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$logRoot = Join-Path $repoRoot (
    "out\university-visual-check\runs\$runTimestamp\logs"
)
$runExecutable = Join-Path $logRoot 'skate3.exe'
$runRuntime = Join-Path $logRoot 'rexruntime.dll'

function Assert-PreparedHash {
    param(
        [Parameter(Mandatory)]
        [string]$Name,
        [Parameter(Mandatory)]
        [string]$Path,
        [Parameter(Mandatory)]
        [string]$Expected
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Prepared $Name is missing: $Path"
    }
    $actual = (
        Get-FileHash -LiteralPath $Path -Algorithm SHA256
    ).Hash
    if ($actual -cne $Expected) {
        throw (
            "Prepared $Name has changed since offline validation. " +
            'Ask Codex to rebuild and stage the University check.'
        )
    }
}

try {
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw (
            'No offline-prepared University build exists. Ask Codex to ' +
            'build and stage it before running this BAT.'
        )
    }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw |
        ConvertFrom-Json
    $executable = Join-Path $preparedRoot 'skate3.exe'
    $runtime = Join-Path $preparedRoot 'rexruntime.dll'
    $package = Join-Path $preparedRoot 'owned_maps\University.skate'
    Assert-PreparedHash 'executable' $executable `
        $manifest.executable_sha256
    Assert-PreparedHash 'runtime' $runtime $manifest.runtime_sha256
    Assert-PreparedHash 'map package' $package $manifest.map_sha256

    $running = Get-Process -Name 'skate3' -ErrorAction SilentlyContinue
    if ($null -ne $running) {
        throw 'Close existing skate3.exe processes before this visual check.'
    }

    New-Item -ItemType Directory -Path $logRoot -Force | Out-Null
    # The application intentionally patches its own executable on first boot
    # to install the title icon extracted from the user's retail game data.
    # Run a disposable copy so that mutation cannot invalidate the immutable,
    # offline-validated staged executable used by later checks.
    Copy-Item -LiteralPath $executable -Destination $runExecutable -Force
    Copy-Item -LiteralPath $runtime -Destination $runRuntime -Force
    Assert-PreparedHash 'run executable copy' $runExecutable `
        $manifest.executable_sha256
    Assert-PreparedHash 'run runtime copy' $runRuntime `
        $manifest.runtime_sha256
    $runtimeLog = Join-Path $logRoot 'skate3_university.log'
    $replaceRetail = if ($RetailCollisionOnly) { 'false' } else { 'true' }
    $retailOnly = if ($RetailCollisionOnly) { 'true' } else { 'false' }
    $arguments = @(
        '--fullscreen=false',
        '--window_width=1280',
        '--window_height=720',
        '--draw_resolution_scale_x=1',
        '--draw_resolution_scale_y=1',
        '--skate3_direct_boot=true',
        '--skate3_mechanics_sandbox=true',
        '--skate3_mechanics_sandbox_visual_map=true',
        '--skate3_mechanics_sandbox_native_collision=true',
        "--skate3_mechanics_sandbox_native_collision_replace_retail=$replaceRetail",
        "--skate3_mechanics_sandbox_native_collision_retail_only=$retailOnly",
        '--skate3_mechanics_sandbox_native_grinds=true',
        '--skate3_native_render=true',
        '--skate3_native_render_scene=true',
        '--skate3_native_render_log_interval=300',
        '--skate3_native_render_scene_perf_log=true',
        '--skate3_native_render_scene_perf_interval=300',
        '--skate3_native_render_scene_perf_items=false',
        '--log_level=info',
        '--log_flush_interval=1',
        '--log_max_file_size_mb=100',
        '--log_max_files=5',
        "--log_file=$runtimeLog"
    )
    [Environment]::SetEnvironmentVariable(
        'SKATE3_OWNED_MAP',
        $package,
        'Process'
    )
    $arguments | Set-Content -LiteralPath (
        Join-Path $logRoot 'launch-arguments.txt'
    ) -Encoding UTF8
    $manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (
        Join-Path $logRoot 'prepared-manifest.json'
    ) -Encoding UTF8

    $collisionMode = if ($RetailCollisionOnly) {
        'retail-only A/B collision'
    } else {
        'owned University collision'
    }
    Write-Host (
        "Launching the offline-prepared University build ($collisionMode)."
    )
    Write-Host "This run's logs: $logRoot"
    $process = Start-Process -FilePath $runExecutable `
        -WorkingDirectory $preparedRoot `
        -ArgumentList $arguments `
        -PassThru `
        -Wait
    if ($process.ExitCode -ne 0) {
        throw "skate3.exe exited with code $($process.ExitCode)"
    }
    Write-Host "University run finished. Logs: $logRoot"
} catch {
    Write-Error $_
    Write-Host "Intended log folder: $logRoot"
    exit 1
} finally {
    # Preserve logs and launch metadata, but discard the self-patched runtime
    # copies. The immutable prepared artifacts remain hash-verifiable.
    foreach ($temporary in @($runExecutable, $runRuntime)) {
        if (Test-Path -LiteralPath $temporary -PathType Leaf) {
            Remove-Item -LiteralPath $temporary -Force `
                -ErrorAction SilentlyContinue
        }
    }
}
