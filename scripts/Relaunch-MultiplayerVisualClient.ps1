[CmdletBinding()]
param(
    [ValidateRange(1, 5)]
    [int]$Role = 3
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot '..')
)
$latestFile = Join-Path $repoRoot 'out\visual-checks\LATEST.txt'
if (-not (Test-Path -LiteralPath $latestFile -PathType Leaf)) {
    throw (
        'No completed visual-check setup was found. Run ' +
        'RUN_MULTIPLAYER_VISUAL_CHECK.bat first.'
    )
}
$runRoot = (Get-Content -LiteralPath $latestFile -Raw).Trim()
$clientRoot = Join-Path $runRoot "clients\client$Role"
$executable = Join-Path $clientRoot 'skate3.exe'
$argumentFile = Join-Path $clientRoot 'launch-arguments.txt'
foreach ($required in @($runRoot, $clientRoot, $executable, $argumentFile)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Latest visual-check run is incomplete: $required"
    }
}

$resolvedExecutable = [System.IO.Path]::GetFullPath($executable)
$alreadyRunning = Get-CimInstance Win32_Process `
    -Filter "Name = 'skate3.exe'" |
    Where-Object {
        -not [string]::IsNullOrWhiteSpace($_.ExecutablePath) -and
        [System.IO.Path]::GetFullPath($_.ExecutablePath) -eq
            $resolvedExecutable
    }
if ($null -ne $alreadyRunning) {
    throw (
        "Client role $Role is still running. Close it before relaunching."
    )
}

$arguments = @(
    Get-Content -LiteralPath $argumentFile |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
)
$process = Start-Process -FilePath $executable `
    -WorkingDirectory $clientRoot `
    -ArgumentList $arguments -PassThru
'{0} {1} {2}' -f (
    Get-Date -Format 'o'
), $process.Id, $resolvedExecutable | Add-Content -LiteralPath (
    Join-Path $runRoot 'relaunches.txt'
) -Encoding UTF8

Write-Host "Relaunched multiplayer client role $Role."
Write-Host "Run folder: $runRoot"
Write-Host (
    'Continue the visual test for at least two minutes, then close all ' +
    'clients and tell the agent the run is complete.'
)
