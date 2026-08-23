#pragma once

#include "skate3_multiplayer_protocol_v12.h"

#include <cstdint>
#include <string_view>

namespace skate3::multiplayer::protocol_v12::live {

struct CompatibilityIdentity {
  std::uint64_t map_hash = 0;
  std::uint64_t build_hash = 0;
  std::uint64_t content_hash = 0;

  [[nodiscard]] constexpr bool operator==(
      const CompatibilityIdentity&) const = default;
};

[[nodiscard]] constexpr std::uint64_t Fnv1a64(
    std::string_view value) {
  std::uint64_t hash = 14695981039346656037ull;
  for (const unsigned char byte : value) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  return hash;
}

// These identify the live compatibility contract, not a particular Git
// commit. They change only when a build or content-schema difference makes
// realtime replication unsafe between peers.
inline constexpr std::uint64_t kBuildCompatibilityHash =
    Fnv1a64(
        "skate3-multiplayer-live-v12-persistent-confirmed-baseline-1");
inline constexpr std::uint64_t kContentCompatibilityHash =
    Fnv1a64("skate3-cac-recipe-final-pose-contract-1");
inline constexpr std::uint64_t kAdvertisedFeatureBits =
    kFeatureExplicitLittleEndian | kFeaturePoseAcknowledgements |
    kFeaturePoseGroups;

[[nodiscard]] constexpr CompatibilityIdentity
MakeCompatibilityIdentity(std::string_view map_name) {
  return {
      .map_hash = Fnv1a64(map_name),
      .build_hash = kBuildCompatibilityHash,
      .content_hash = kContentCompatibilityHash,
  };
}

[[nodiscard]] constexpr Capabilities MakeCapabilities(
    const CompatibilityIdentity& identity) {
  return {
      .feature_bits = kAdvertisedFeatureBits,
      .map_hash = identity.map_hash,
      .build_hash = identity.build_hash,
      .content_hash = identity.content_hash,
      .maximum_datagram_bytes = kMaximumDatagramBytes,
      .maximum_pose_groups = 1,
      .reserved = 0,
  };
}

[[nodiscard]] constexpr bool CapabilityEnvelopeShapeValid(
    const Envelope& envelope) {
  return EnvelopeShapeValid(envelope) &&
         envelope.kind == MessageKind::kCapabilities &&
         envelope.flags == kFlagReliable &&
         envelope.stream_id == 0 &&
         envelope.payload_bytes == kCapabilitiesPayloadBytes;
}

}  // namespace skate3::multiplayer::protocol_v12::live
