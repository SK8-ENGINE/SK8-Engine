#pragma once

#include <cstdint>

namespace skate3::multiplayer::bandwidth {

// Internet baseline for the complete final-pose path. Root motion remains at
// 60 Hz; the much larger skeleton is sampled at 20 Hz and reconstructed at
// render cadence. The byte budget includes root/control traffic and current
// v12 datagram headers, but not Steam's outer transport framing.
inline constexpr std::int32_t kRootSnapshotRateHz = 60;
inline constexpr std::int32_t kAnimationSnapshotRateHz = 20;
// Zero removes the additional configured floor. Playback still remains
// bounded to a completed animation interval, which measured about 75 ms of
// natural look-behind in the five-client localhost acceptance run.
inline constexpr std::int32_t kMinimumInterpolationDelayMs = 0;
inline constexpr double kPerPeerApplicationBudgetBytesPerSecond =
    112.0 * 1024.0;

[[nodiscard]] constexpr double DirectMeshUploadBytesPerSecond(
    std::uint32_t participant_count,
    double per_peer_bytes_per_second =
        kPerPeerApplicationBudgetBytesPerSecond) {
  return participant_count < 2
             ? 0.0
             : per_peer_bytes_per_second *
                   static_cast<double>(participant_count - 1);
}

[[nodiscard]] constexpr double BitsPerSecond(double bytes_per_second) {
  return bytes_per_second * 8.0;
}

}  // namespace skate3::multiplayer::bandwidth
