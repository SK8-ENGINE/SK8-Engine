#pragma once

#include <cstdint>

namespace skate3::multiplayer::bandwidth {

// Internet baseline for the complete final-pose path. Root motion remains at
// 60 Hz; the much larger skeleton is sampled at 20 Hz and reconstructed at
// render cadence. The byte budget includes root/control traffic and current
// v12 datagram headers, but not Steam's outer transport framing.
inline constexpr std::int32_t kRootSnapshotRateHz = 60;
inline constexpr std::int32_t kAnimationSnapshotRateHz = 20;
inline constexpr std::int32_t kMinimumInterpolationDelayMs = 100;
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
