param(
    [ValidateRange(1, 1440)]
    [int]$Minutes = 10,
    [ValidateSet('Release', 'RelWithDebInfo', 'Debug')]
    [string]$BuildType = 'Release'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildRoot = Join-Path $repoRoot 'out\build\multiplayer-tests'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$runRoot = Join-Path $repoRoot "out\synthetic-soaks\$stamp"
New-Item -ItemType Directory -Path $runRoot -Force | Out-Null
$logPath = Join-Path $runRoot 'soak.log'
$summaryPath = Join-Path $runRoot 'summary.txt'

function Invoke-Logged {
    param(
        [Parameter(Mandatory)][string]$FilePath,
        [Parameter(Mandatory)][string[]]$Arguments
    )

    "COMMAND: $FilePath $($Arguments -join ' ')" |
        Tee-Object -FilePath $logPath -Append
    & $FilePath @Arguments 2>&1 |
        Tee-Object -FilePath $logPath -Append
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath failed with exit code $LASTEXITCODE"
    }
}

try {
    Invoke-Logged -FilePath 'cmake' -Arguments @(
        '-S', (Join-Path $repoRoot 'tests'),
        '-B', $buildRoot,
        '-G', 'Ninja',
        "-DCMAKE_BUILD_TYPE=$BuildType"
    )
    Invoke-Logged -FilePath 'cmake' -Arguments @(
        '--build', $buildRoot, '--parallel'
    )

    $deadline = [DateTime]::UtcNow.AddMinutes($Minutes)
    $iterations = 0
    $started = [DateTime]::UtcNow
    while ([DateTime]::UtcNow -lt $deadline) {
        $iterations++
        "SOAK ITERATION $iterations" |
            Tee-Object -FilePath $logPath -Append
        Invoke-Logged -FilePath 'ctest' -Arguments @(
            '--test-dir', $buildRoot,
            '--output-on-failure',
            '-R',
            (
                'protocol_v12_(state|transport|lossless|delta|' +
                'block_delta|snappy)|outbound_scheduler|lifecycle|' +
                'worker|routing|scale'
            )
        )
    }
    $elapsed = [DateTime]::UtcNow - $started
    @(
        'MULTIPLAYER SYNTHETIC SOAK PASSED'
        "repository=$repoRoot"
        "build_type=$BuildType"
        "requested_minutes=$Minutes"
        ('elapsed_seconds={0:0.000}' -f $elapsed.TotalSeconds)
        "completed_iterations=$iterations"
        'game_clients_launched=0'
        "log=$logPath"
    ) | Set-Content -LiteralPath $summaryPath -Encoding UTF8
    Get-Content -LiteralPath $summaryPath
} catch {
    @(
        'MULTIPLAYER SYNTHETIC SOAK FAILED'
        "repository=$repoRoot"
        "build_type=$BuildType"
        "requested_minutes=$Minutes"
        "error=$($_.Exception.Message)"
        "log=$logPath"
    ) | Set-Content -LiteralPath $summaryPath -Encoding UTF8
    Get-Content -LiteralPath $summaryPath
    exit 1
}
