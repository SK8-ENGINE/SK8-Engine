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
        [AllowEmptyCollection()]
        [AllowEmptyString()]
        [string[]]$Lines,
        [Parameter(Mandatory)][string]$Pattern
    )

    return @($Lines | Select-String -Pattern $Pattern).Count
}

function Maximum-IntegerField {
    param(
        [Parameter(Mandatory)]
        [AllowEmptyCollection()]
        [AllowEmptyString()]
        [string[]]$Lines,
        [Parameter(Mandatory)][string]$Name
    )

    $maximum = $null
    $pattern = [regex]::Escape($Name) + '[=:](\d+)'
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

function Maximum-DecimalField {
    param(
        [Parameter(Mandatory)]
        [AllowEmptyCollection()]
        [AllowEmptyString()]
        [string[]]$Lines,
        [Parameter(Mandatory)][string]$Name
    )

    $maximum = $null
    $pattern = [regex]::Escape($Name) + '=([0-9]+(?:\.[0-9]+)?)'
    foreach ($line in $Lines) {
        $match = [regex]::Match($line, $pattern)
        if ($match.Success) {
            $value = [double]::Parse(
                $match.Groups[1].Value,
                [Globalization.CultureInfo]::InvariantCulture
            )
            if ($null -eq $maximum -or $value -gt $maximum) {
                $maximum = $value
            }
        }
    }
    if ($null -eq $maximum) {
        return 'n/a'
    }
    return $maximum.ToString(
        '0.###', [Globalization.CultureInfo]::InvariantCulture
    )
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
    $peerTimingLines = @(
        $lines | Select-String -Pattern 'multiplayer-peer-timing:' |
            ForEach-Object { $_.Line }
    )
    $peerMotionLines = @(
        $lines | Select-String -Pattern 'multiplayer-peer-motion:' |
            ForEach-Object { $_.Line }
    )
    $renderHandoffLines = @(
        $lines | Select-String -Pattern 'multiplayer-render-handoff:' |
            ForEach-Object { $_.Line }
    )
    $renderMotionLines = @(
        $lines | Select-String -Pattern 'multiplayer-render-motion:' |
            ForEach-Object { $_.Line }
    )
    $visibleMotionLines = @(
        $lines | Select-String -Pattern 'multiplayer-visible-motion:' |
            ForEach-Object { $_.Line }
    )
    $captureVisibleMotionLines = @(
        $visibleMotionLines |
            Select-String -Pattern ' stage=capture(?:\s|$)' |
            ForEach-Object { $_.Line }
    )
    $appliedVisibleMotionLines = @(
        $visibleMotionLines |
            Select-String -Pattern ' stage=applied(?:\s|$)' |
            ForEach-Object { $_.Line }
    )
    $poseCadenceLines = @(
        $lines | Select-String -Pattern 'multiplayer-pose-cadence:' |
            ForEach-Object { $_.Line }
    )
    $gpuUploadRingLines = @(
        $lines |
            Select-String -Pattern 'multiplayer-gpu-upload-ring:' |
            ForEach-Object { $_.Line }
    )
    $captureCadenceLines = @(
        $poseCadenceLines |
            Select-String -Pattern ' stage=capture(?:\s|$)' |
            ForEach-Object { $_.Line }
    )
    $interpolatedCadenceLines = @(
        $poseCadenceLines |
            Select-String -Pattern ' stage=interpolated(?:\s|$)' |
            ForEach-Object { $_.Line }
    )
    $appliedCadenceLines = @(
        $poseCadenceLines |
            Select-String -Pattern ' stage=applied(?:\s|$)' |
            ForEach-Object { $_.Line }
    )
    $perfLines = @(
        $lines | Select-String -Pattern 'multiplayer-perf:' |
            ForEach-Object { $_.Line }
    )
    $workerLines = @(
        $lines | Select-String -Pattern 'multiplayer-worker:' |
            ForEach-Object { $_.Line }
    )
    $renderCacheRuntimeLines = @(
        $lines |
            Select-String -Pattern (
                'multiplayer-render-cache: frames='
            ) |
            ForEach-Object { $_.Line }
    )
    $appearancePrepareLines = @(
        $lines |
            Select-String -Pattern (
                'multiplayer-appearance-prepare:'
            ) |
            ForEach-Object { $_.Line }
    )
    $appearanceReadyLines = @(
        $appearancePrepareLines |
            Select-String -Pattern 'state=ready' |
            ForEach-Object { $_.Line }
    )
    $appearanceInstallLines = @(
        $lines |
            Select-String -Pattern (
                'multiplayer-appearance-install:'
            ) |
            ForEach-Object { $_.Line }
    )
    $appearanceInstallStepLines = @(
        $lines |
            Select-String -Pattern (
                'multiplayer-appearance-install-step:'
            ) |
            ForEach-Object { $_.Line }
    )
    $appearanceInstallCancelLines = @(
        $lines |
            Select-String -Pattern (
                'multiplayer-appearance-install-cancel:'
            ) |
            ForEach-Object { $_.Line }
    )
    $appearanceResourceLines = @(
        $lines |
            Select-String -Pattern (
                'multiplayer-appearance-resources:'
            ) |
            ForEach-Object { $_.Line }
    )
    $appearanceResourceFaultLines = @(
        $appearanceResourceLines |
            Select-String -Pattern 'faults=[1-9][0-9]*' |
            ForEach-Object { $_.Line }
    )
    $appearanceReleaseLines = @(
        $lines |
            Select-String -Pattern (
                'multiplayer: released renderer appearance role='
            ) |
            ForEach-Object { $_.Line }
    )
    $appearanceReleaseFaultLines = @(
        $appearanceReleaseLines |
            Select-String -Pattern 'faults=[1-9][0-9]*' |
            ForEach-Object { $_.Line }
    )
    $hairRouteLines = @(
        $lines |
            Select-String -Pattern (
                'multiplayer-animation-route: .*kind=hair'
            ) |
            ForEach-Object { $_.Line }
    )
    $appearanceNormalizationLines = @(
        $lines |
            Select-String -Pattern (
                'multiplayer-appearance-normalize:'
            ) |
            ForEach-Object { $_.Line }
    )
    $droppedGarmentLines = @(
        $lines |
            Select-String -Pattern (
                'ropa rescue DROPPED-GARMENT suppressed'
            ) |
            ForEach-Object { $_.Line }
    )
    $ropaBindingLines = @(
        $lines |
            Select-String -Pattern (
                'multiplayer-appearance-binding: .*kind=ropa'
            ) |
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
    $summary.Add("peer_timing_samples=$($peerTimingLines.Count)")
    $summary.Add("peer_motion_samples=$($peerMotionLines.Count)")
    foreach ($sender in 1..5) {
        $senderTimingLines = @(
            $peerTimingLines |
                Select-String -Pattern (
                    " sender=$sender(?:\s|$)"
                ) |
                ForEach-Object { $_.Line }
        )
        if ($senderTimingLines.Count -gt 0) {
            $summary.Add(
                "last_peer_timing_sender_$sender=" +
                $senderTimingLines[-1]
            )
        }
        $activeSenderTimingLines = @(
            $senderTimingLines | Where-Object {
                $match = [regex]::Match($_, ' rx=([0-9.]+)fps')
                $match.Success -and
                    [double]$match.Groups[1].Value -ge 50.0
            }
        )
        if ($activeSenderTimingLines.Count -gt 0) {
            $summary.Add(
                "last_active_peer_timing_sender_$sender=" +
                $activeSenderTimingLines[-1]
            )
        }
        $senderMotionLines = @(
            $peerMotionLines |
                Select-String -Pattern (
                    " sender=$sender(?:\s|$)"
                ) |
                ForEach-Object { $_.Line }
        )
        if ($senderMotionLines.Count -gt 0) {
            $summary.Add(
                "last_peer_motion_sender_$sender=" +
                $senderMotionLines[-1]
            )
        }
    }
    $summary.Add("multiplayer_perf_samples=$($perfLines.Count)")
    $summary.Add("multiplayer_worker_samples=$($workerLines.Count)")
    $summary.Add(
        "multiplayer_render_cache_samples=" +
        $renderCacheRuntimeLines.Count
    )
    $summary.Add(
        "multiplayer_render_handoff_samples=" +
        $renderHandoffLines.Count
    )
    $summary.Add(
        "multiplayer_render_motion_samples=" +
        $renderMotionLines.Count
    )
    $summary.Add(
        "multiplayer_visible_motion_samples=" +
        $visibleMotionLines.Count
    )
    $summary.Add(
        "multiplayer_pose_cadence_samples=" +
        $poseCadenceLines.Count
    )
    $summary.Add(
        "multiplayer_gpu_upload_ring_samples=" +
        $gpuUploadRingLines.Count
    )
    $summary.Add(
        'max_gpu_upload_unsafe_reuse=' +
        (Maximum-IntegerField $gpuUploadRingLines 'total')
    )
    $summary.Add(
        'max_gpu_upload_busy_regions=' +
        (Maximum-IntegerField $gpuUploadRingLines 'busy')
    )
    $summary.Add(
        "appearance_prepare_events=" +
        $appearancePrepareLines.Count
    )
    $summary.Add(
        "appearance_prepare_ready=" +
        $appearanceReadyLines.Count
    )
    $summary.Add(
        'appearance_prepare_failed=' +
        (Match-Count $appearancePrepareLines 'state=failed')
    )
    $summary.Add(
        'appearance_prepare_stale=' +
        (Match-Count $appearancePrepareLines 'state=stale')
    )
    $summary.Add(
        'max_appearance_prepare_ms=' +
        (Maximum-DecimalField $appearanceReadyLines 'prepare')
    )
    $summary.Add(
        "appearance_gpu_install_events=" +
        $appearanceInstallLines.Count
    )
    $summary.Add(
        'max_appearance_gpu_install_ms=' +
        (Maximum-DecimalField $appearanceInstallLines 'upload')
    )
    $summary.Add(
        'appearance_gpu_install_steps=' +
        $appearanceInstallStepLines.Count
    )
    $summary.Add(
        'appearance_gpu_install_cancels=' +
        $appearanceInstallCancelLines.Count
    )
    $summary.Add(
        'max_appearance_gpu_step_ms=' +
        (Maximum-DecimalField $appearanceInstallStepLines 'upload')
    )
    $summary.Add(
        'max_appearance_gpu_total_ms=' +
        (Maximum-DecimalField $appearanceInstallLines 'total')
    )
    $summary.Add(
        'max_appearance_gpu_wall_ms=' +
        (Maximum-DecimalField $appearanceInstallLines 'wall')
    )
    $summary.Add(
        'max_appearance_gpu_install_frames=' +
        (Maximum-IntegerField $appearanceInstallLines 'frames')
    )
    $summary.Add(
        'max_appearance_gpu_install_operations=' +
        (Maximum-IntegerField $appearanceInstallLines 'operations')
    )
    $summary.Add(
        'appearance_resource_audits=' +
        $appearanceResourceLines.Count
    )
    $summary.Add(
        'appearance_resource_fault_events=' +
        $appearanceResourceFaultLines.Count
    )
    $summary.Add(
        'max_appearance_installed_roles=' +
        (Maximum-IntegerField $appearanceResourceLines 'installed_roles')
    )
    $summary.Add(
        'max_appearance_pending_roles=' +
        (Maximum-IntegerField $appearanceResourceLines 'pending_roles')
    )
    $summary.Add(
        'max_appearance_installed_meshes=' +
        (Maximum-IntegerField $appearanceResourceLines 'installed_meshes')
    )
    $summary.Add(
        'max_appearance_pending_meshes=' +
        (Maximum-IntegerField $appearanceResourceLines 'pending_meshes')
    )
    $summary.Add(
        'max_appearance_installed_textures=' +
        (Maximum-IntegerField $appearanceResourceLines 'installed_textures')
    )
    $summary.Add(
        'max_appearance_pending_textures=' +
        (Maximum-IntegerField $appearanceResourceLines 'pending_textures')
    )
    $summary.Add(
        'remote_hair_route_events=' +
        $hairRouteLines.Count
    )
    $summary.Add(
        'remote_appearance_fade_normalizations=' +
        $appearanceNormalizationLines.Count
    )
    $summary.Add(
        'local_dropped_garment_suppressions=' +
        $droppedGarmentLines.Count
    )
    $summary.Add(
        'remote_ropa_binding_events=' +
        $ropaBindingLines.Count
    )
    $summary.Add(
        'local_ropa_hair_rigid_guards=' +
        (Match-Count $lines (
            'native-scene: ropa hair rigid guard'
        ))
    )
    $summary.Add(
        'local_ropa_hair_rigid_rescues=' +
        (Match-Count $lines (
            'native-scene: ropa hair rigid rescue'
        ))
    )
    $summary.Add(
        'max_socket_failures=' +
        (Maximum-IntegerField $rateLines 'failures')
    )
    $summary.Add(
        'max_rejected_packets=' +
        (Maximum-IntegerField $rateLines 'rejected')
    )
    $summary.Add(
        'transport_policy_events=' +
        (Match-Count $lines (
            'multiplayer: transport policy root=unreliable ' +
            'animation=unreliable control=reliable ' +
            'appearance=reliable'
        ))
    )
    $summary.Add(
        'max_animation_unreliable_fragments=' +
        (Maximum-IntegerField $rateLines 'anim_u')
    )
    $summary.Add(
        'max_appearance_reliable_chunks=' +
        (Maximum-IntegerField $rateLines 'appearance_r')
    )
    $summary.Add(
        'max_control_reliable_packets=' +
        (Maximum-IntegerField $rateLines 'control_r')
    )
    $summary.Add(
        'max_delivery_policy_errors=' +
        (Maximum-IntegerField $rateLines 'errors')
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
        'v12_capability_events=' +
        (Match-Count $lines 'multiplayer-v12: peer role=')
    )
    $summary.Add(
        'max_v12_capabilities_sent=' +
        (Maximum-IntegerField $rateLines 'v12_tx')
    )
    $summary.Add(
        'max_v12_capabilities_received=' +
        (Maximum-IntegerField $rateLines 'v12_rx')
    )
    $summary.Add(
        'max_v12_capability_peers=' +
        (Maximum-IntegerField $rateLines 'v12_peers')
    )
    $summary.Add(
        'max_v12_capability_rejections=' +
        (Maximum-IntegerField $rateLines 'v12_rejected')
    )
    $summary.Add(
        'max_v12_capability_incompatibilities=' +
        (Maximum-IntegerField $rateLines 'v12_incompatible')
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
        'appearance_fanout_restarts=' +
        (Match-Count $lines (
            'multiplayer: restarted localhost appearance fanout peer='
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
        'renderer_release_events=' +
        $appearanceReleaseLines.Count
    )
    $summary.Add(
        'renderer_release_fault_events=' +
        $appearanceReleaseFaultLines.Count
    )
    $summary.Add(
        'max_renderer_release_faults=' +
        (Maximum-IntegerField $appearanceReleaseLines 'faults')
    )
    $summary.Add(
        'renderer_cache_prepare_events=' +
        (Match-Count $lines (
            'multiplayer-render-cache: prepared role='
        ))
    )
    $summary.Add(
        'max_renderer_cache_weighted_fallbacks=' +
        $(if ($renderCacheRuntimeLines.Count -gt 0) {
            Maximum-IntegerField (
                $renderCacheRuntimeLines
            ) 'weighted_fallbacks'
        } else {
            'n/a'
        })
    )
    $summary.Add(
        'max_renderer_cache_rig_retries=' +
        $(if ($renderCacheRuntimeLines.Count -gt 0) {
            Maximum-IntegerField (
                $renderCacheRuntimeLines
            ) 'rig_retries'
        } else {
            'n/a'
        })
    )
    $summary.Add(
        'local_capture_ready_events=' +
        (Match-Count $lines (
            'multiplayer-local-capture: state=ready'
        ))
    )
    $summary.Add(
        'local_capture_missing_events=' +
        (Match-Count $lines (
            'multiplayer-local-capture: state=missing'
        ))
    )
    $summary.Add(
        'local_capture_far_exact_events=' +
        (Match-Count $lines (
            'multiplayer-local-capture: exact entity accepted beyond'
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
    $summary.Add(
        'last_multiplayer_perf=' +
        $(if ($perfLines.Count -gt 0) {
            $perfLines[-1]
        } else {
            'missing'
        })
    )
    $summary.Add(
        'last_multiplayer_worker=' +
        $(if ($workerLines.Count -gt 0) {
            $workerLines[-1]
        } else {
            'missing'
        })
    )
    $summary.Add(
        'last_multiplayer_render_cache=' +
        $(if ($renderCacheRuntimeLines.Count -gt 0) {
            $renderCacheRuntimeLines[-1]
        } else {
            'missing'
        })
    )
    $summary.Add(
        'last_multiplayer_render_handoff=' +
        $(if ($renderHandoffLines.Count -gt 0) {
            $renderHandoffLines[-1]
        } else {
            'missing'
        })
    )
    $summary.Add(
        'last_pose_capture=' +
        $(if ($captureCadenceLines.Count -gt 0) {
            $captureCadenceLines[-1]
        } else {
            'missing'
        })
    )
    $summary.Add(
        'last_visible_motion_capture=' +
        $(if ($captureVisibleMotionLines.Count -gt 0) {
            $captureVisibleMotionLines[-1]
        } else {
            'missing'
        })
    )
    $summary.Add(
        'last_pose_interpolated=' +
        $(if ($interpolatedCadenceLines.Count -gt 0) {
            $interpolatedCadenceLines[-1]
        } else {
            'missing'
        })
    )
    $summary.Add(
        'last_pose_applied=' +
        $(if ($appliedCadenceLines.Count -gt 0) {
            $appliedCadenceLines[-1]
        } else {
            'missing'
        })
    )
    $summary.Add(
        'last_gpu_upload_ring=' +
        $(if ($gpuUploadRingLines.Count -gt 0) {
            $gpuUploadRingLines[-1]
        } else {
            'missing'
        })
    )
    foreach ($sender in 1..5) {
        $senderRenderMotionLines = @(
            $renderMotionLines |
                Select-String -Pattern (
                    " role=$sender(?:\s|$)"
                ) |
                ForEach-Object { $_.Line }
        )
        if ($senderRenderMotionLines.Count -gt 0) {
            $summary.Add(
                "last_render_motion_sender_$sender=" +
                $senderRenderMotionLines[-1]
            )
        }
        $senderVisibleMotionLines = @(
            $appliedVisibleMotionLines |
                Select-String -Pattern (
                    " role=$sender(?:\s|$)"
                ) |
                ForEach-Object { $_.Line }
        )
        if ($senderVisibleMotionLines.Count -gt 0) {
            $summary.Add(
                "last_visible_motion_sender_$sender=" +
                $senderVisibleMotionLines[-1]
            )
        }
        $senderInterpolatedCadenceLines = @(
            $interpolatedCadenceLines |
                Select-String -Pattern (
                    " role=$sender(?:\s|$)"
                ) |
                ForEach-Object { $_.Line }
        )
        if ($senderInterpolatedCadenceLines.Count -gt 0) {
            $summary.Add(
                "last_pose_interpolated_sender_$sender=" +
                $senderInterpolatedCadenceLines[-1]
            )
        }
        $senderAppliedCadenceLines = @(
            $appliedCadenceLines |
                Select-String -Pattern (
                    " role=$sender(?:\s|$)"
                ) |
                ForEach-Object { $_.Line }
        )
        if ($senderAppliedCadenceLines.Count -gt 0) {
            $summary.Add(
                "last_pose_applied_sender_$sender=" +
                $senderAppliedCadenceLines[-1]
            )
        }
    }
    $summary.Add(
        'last_appearance_prepare=' +
        $(if ($appearancePrepareLines.Count -gt 0) {
            $appearancePrepareLines[-1]
        } else {
            'missing'
        })
    )
    $summary.Add(
        'last_appearance_gpu_install=' +
        $(if ($appearanceInstallLines.Count -gt 0) {
            $appearanceInstallLines[-1]
        } else {
            'missing'
        })
    )
    $summary.Add(
        'last_appearance_gpu_install_step=' +
        $(if ($appearanceInstallStepLines.Count -gt 0) {
            $appearanceInstallStepLines[-1]
        } else {
            'missing'
        })
    )
    $summary.Add(
        'last_appearance_gpu_install_cancel=' +
        $(if ($appearanceInstallCancelLines.Count -gt 0) {
            $appearanceInstallCancelLines[-1]
        } else {
            'missing'
        })
    )
    $summary.Add(
        'last_appearance_resource_audit=' +
        $(if ($appearanceResourceLines.Count -gt 0) {
            $appearanceResourceLines[-1]
        } else {
            'missing'
        })
    )
    $summary.Add(
        'last_remote_hair_route=' +
        $(if ($hairRouteLines.Count -gt 0) {
            $hairRouteLines[-1]
        } else {
            'missing'
        })
    )
    $summary.Add(
        'last_remote_appearance_normalization=' +
        $(if ($appearanceNormalizationLines.Count -gt 0) {
            $appearanceNormalizationLines[-1]
        } else {
            'missing'
        })
    )
    $summary.Add(
        'last_local_dropped_garment_suppression=' +
        $(if ($droppedGarmentLines.Count -gt 0) {
            $droppedGarmentLines[-1]
        } else {
            'missing'
        })
    )
    $summary.Add(
        'last_remote_ropa_binding=' +
        $(if ($ropaBindingLines.Count -gt 0) {
            $ropaBindingLines[-1]
        } else {
            'missing'
        })
    )
    $summary.Add('')
}

$summaryPath = Join-Path $runRoot 'telemetry-summary.txt'
$summary | Set-Content -LiteralPath $summaryPath -Encoding UTF8
$summary | ForEach-Object { Write-Output $_ }
Write-Output "Summary written to: $summaryPath"
