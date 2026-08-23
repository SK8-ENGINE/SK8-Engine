[CmdletBinding()]
param(
    [string]$RunDirectory = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot '..')
)
if ([string]::IsNullOrWhiteSpace($RunDirectory)) {
    $latestFile = Join-Path $repoRoot 'out\visual-checks\LATEST.txt'
    if (-not (Test-Path -LiteralPath $latestFile -PathType Leaf)) {
        throw 'No latest multiplayer visual-check run was found.'
    }
    $RunDirectory = (
        Get-Content -LiteralPath $latestFile -Raw
    ).Trim()
}
$runRoot = [System.IO.Path]::GetFullPath($RunDirectory)
$clientsRoot = Join-Path $runRoot 'clients'
if (-not (Test-Path -LiteralPath $clientsRoot -PathType Container)) {
    throw "Visual-check client directory is missing: $clientsRoot"
}

function Match-Count {
    param(
        [Parameter(Mandatory)]
        [AllowEmptyString()]
        [string[]]$Lines,
        [Parameter(Mandatory)][string]$Pattern
    )

    return @($Lines | Select-String -Pattern $Pattern).Count
}

function Maximum-IntegerField {
    param(
        [Parameter(Mandatory)]
        [AllowEmptyString()]
        [string[]]$Lines,
        [Parameter(Mandatory)][string]$Name
    )

    $maximum = $null
    $pattern = [regex]::Escape($Name) + '=(\d+)'
    foreach ($line in $Lines) {
        $match = [regex]::Match($line, $pattern)
        if ($match.Success) {
            $value = [int64]$match.Groups[1].Value
            if ($null -eq $maximum -or $value -gt $maximum) {
                $maximum = $value
            }
        }
    }
    if ($null -eq $maximum) {
        return 'n/a'
    }
    return [string]$maximum
}

$summary = New-Object System.Collections.Generic.List[string]
$summary.Add('MULTIPLAYER TELEMETRY SUMMARY')
$summary.Add("Run: $runRoot")
$summary.Add(
    'This report does not establish visual correctness; the user reports ' +
    'that result separately.'
)
$summary.Add('')

$clientDirectories = Get-ChildItem -LiteralPath $clientsRoot `
    -Directory | Sort-Object Name
foreach ($client in $clientDirectories) {
    $logsRoot = Join-Path $client.FullName 'logs'
    $logs = @()
    if (Test-Path -LiteralPath $logsRoot -PathType Container) {
        $logs = @(
            Get-ChildItem -LiteralPath $logsRoot -File -Filter '*.log' |
                Sort-Object LastWriteTime
        )
    }
    $summary.Add("[$($client.Name)]")
    if ($logs.Count -eq 0) {
        $summary.Add('log=missing')
        $summary.Add('')
        continue
    }

    $lines = @($logs | ForEach-Object {
        Get-Content -LiteralPath $_.FullName
    })
    $rateLines = @(
        $lines | Select-String -Pattern 'multiplayer-net:' |
            ForEach-Object { $_.Line }
    )
    $lastRate = if ($rateLines.Count -gt 0) {
        $rateLines[-1]
    } else {
        'missing'
    }

    $summary.Add("logs=$($logs.Count)")
    $summary.Add("latest_log=$($logs[-1].FullName)")
    $summary.Add("rate_samples=$($rateLines.Count)")
    $summary.Add(
        'max_socket_failures=' +
        (Maximum-IntegerField $rateLines 'failures')
    )
    $summary.Add(
        'max_rejected_packets=' +
        (Maximum-IntegerField $rateLines 'rejected')
    )
    $summary.Add(
        'max_known_peers=' +
        (Maximum-IntegerField $rateLines 'peers')
    )
    $summary.Add(
        'max_visible_players=' +
        (Maximum-IntegerField $rateLines 'visible')
    )
    $summary.Add(
        'capability_events=' +
        (Match-Count $lines 'multiplayer: peer role=.*capabilities=')
    )
    $summary.Add(
        'appearance_byte_receipts=' +
        (Match-Count $lines 'multiplayer: peer role=.*appearance .*state=1')
    )
    $summary.Add(
        'appearance_install_receipts=' +
        (Match-Count $lines 'multiplayer: peer role=.*appearance .*state=2')
    )
    $summary.Add(
        'appearance_requests_queued=' +
        (Match-Count $lines 'multiplayer: queued appearance request role=')
    )
    $summary.Add(
        'appearance_resends_started=' +
        (Match-Count $lines (
            'multiplayer: restarted appearance stream requester='
        ))
    )
    $summary.Add(
        'stale_appearance_requests=' +
        (Match-Count $lines (
            'multiplayer: ignored stale appearance request role='
        ))
    )
    $summary.Add(
        'appearance_test_dropped_chunks=' +
        (Match-Count $lines (
            'multiplayer-test: dropped appearance chunk role='
        ))
    )
    $summary.Add(
        'appearance_test_drop_releases=' +
        (Match-Count $lines (
            'multiplayer-test: released appearance drop role='
        ))
    )
    $summary.Add(
        'appearance_receive_events=' +
        (Match-Count $lines 'multiplayer: received appearance role=')
    )
    $summary.Add(
        'renderer_install_events=' +
        (Match-Count $lines (
            'multiplayer: installed (recipe )?appearance role='
        ))
    )
    $summary.Add(
        'local_profile_recipe_updates=' +
        (Match-Count $lines (
            'multiplayer-assets: adopted local profile recipe'
        ))
    )
    $summary.Add(
        'local_recipe_builds=' +
        (Match-Count $lines (
            'multiplayer: built recipe appearance'
        ))
    )
    $summary.Add(
        'presentation_candidate_removals=' +
        (Match-Count $lines (
            'local presentation candidate removed'
        ))
    )
    $summary.Add(
        'presentation_candidate_selections=' +
        (Match-Count $lines (
            'provisional local presentation candidate='
        ))
    )
    $summary.Add(
        'remote_proxy_transitions=' +
        (Match-Count $lines (
            'multiplayer-visual-state: .*mode=proxy'
        ))
    )
    $summary.Add(
        'remote_appearance_transitions=' +
        (Match-Count $lines (
            'multiplayer-visual-state: .*mode=appearance'
        ))
    )
    $summary.Add(
        'incomplete_recipe_events=' +
        (Match-Count $lines (
            'multiplayer: recipe appearance incomplete'
        ))
    )
    $summary.Add(
        'peer_reset_events=' +
        (Match-Count $lines (
            'multiplayer: reset outbound state for role'
        ))
    )
    $summary.Add(
        'multiplayer_error_lines=' +
        (Match-Count $lines (
            '(?i)(\[(error|critical|fatal)\].*multiplayer|' +
            'multiplayer.*\b(error|failed|fatal)\b)'
        ))
    )
    $summary.Add("last_rate=$lastRate")
    $summary.Add('')
}

$summaryPath = Join-Path $runRoot 'telemetry-summary.txt'
$summary | Set-Content -LiteralPath $summaryPath -Encoding UTF8
$summary | ForEach-Object { Write-Output $_ }
Write-Output "Summary written to: $summaryPath"
