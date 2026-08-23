#include "skate3_multiplayer.h"

#include "skate3_multiplayer_lifecycle.h"
#include "skate3_multiplayer_protocol.h"
#include "skate3_steam_backend.h"
#include "skate3_trick_pipeline.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <ostream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#if defined(_WIN32)
#define NOMINMAX
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Mstcpip.h>
#include <Windows.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#endif

REXCVAR_DEFINE_BOOL(
    skate3_multiplayer_local_visuals, false, "Skate 3",
    "Experimental multi-client visual replication over localhost. This sends "
    "only the verified local board pose and does not add remote collision or "
    "gameplay authority.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    skate3_multiplayer_local_client, 0, "Skate 3",
    "Local multiplayer client slot: 0 disables networking, 1 binds the first "
    "localhost port as logical host, and 2-100 join that host.")
    .range(0, 100)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    skate3_multiplayer_local_base_port, 27051, "Skate 3",
    "First localhost UDP port used by the local multiplayer transport.")
    .range(1024, 65436);
REXCVAR_DEFINE_DOUBLE(
    skate3_multiplayer_local_lane_spacing, 0.0, "Skate 3",
    "Optional debugging-only visual separation between local clients. Keep "
    "this at zero for spatially accurate replication near walls and ramps.")
    .range(0.0, 20.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    skate3_multiplayer_quality_preset, 2, "Skate 3",
    "Multiplayer network quality: 0=Auto, 1=Bandwidth Saver, 2=Balanced, "
    "3=High Fidelity, 4=Custom. Auto selects a preset from the live session "
    "size; Custom uses the individual multiplayer tuning cvars.")
    .range(0, 4)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    skate3_multiplayer_local_send_rate, 60, "Skate 3",
    "Custom-preset root pose packets sent per second by the multiplayer "
    "transport.")
    .range(10, 120)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    skate3_multiplayer_local_animation_rate, 60, "Skate 3",
    "Custom-preset completed skeletal-pose frames sent per second by the "
    "multiplayer transport.")
    .range(10, 60)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    skate3_multiplayer_local_interpolation_ms, 50, "Skate 3",
    "Minimum remote-pose buffer duration. Skeletal animation automatically "
    "retains at least two animation frames plus measured network jitter; "
    "larger values trade responsiveness for additional stability.")
    .range(0, 250)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    skate3_multiplayer_animation_interpolation_mode, 2, "Skate 3",
    "Remote skeletal interpolation diagnostic: 0 holds the latest complete "
    "animation frame, 1 interpolates affine rotation/scale with linear "
    "translation, and 2 uses pivot-preserving rigid interpolation.")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    skate3_multiplayer_relevance_radius, 80.0, "Skate 3",
    "Maximum map-space distance for high-detail remote animation routing by "
    "the transport.")
    .range(5.0, 1000.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    skate3_multiplayer_attachment_radius, 35.0, "Skate 3",
    "Distance inside which exact hat, hair, board and wheel attachment "
    "tracks are sent. More distant skaters retain the canonical body "
    "skeleton and use receiver-side attachment remaps.")
    .range(5.0, 250.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    skate3_multiplayer_relevance_players, 12, "Skate 3",
    "Maximum nearest high-detail remote players routed to each client. "
    "Cheap root-presence updates continue outside this budget.")
    .range(1, 32)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    skate3_multiplayer_far_presence_rate, 2, "Skate 3",
    "Root-pose updates per second for players outside the high-detail "
    "relevance set.")
    .range(1, 10)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace skate3::multiplayer {
namespace {

using Clock = std::chrono::steady_clock;
using protocol::AnimationFragmentByteCount;
using protocol::AnimationFragmentPacket;
using protocol::AnimationTrackEncoding;
using protocol::AppearanceFragmentByteCount;
using protocol::AppearanceFragmentPacket;
using protocol::PosePacket;
using protocol::SequenceNewer;
using protocol::kAnimationFragmentWords;
using protocol::kAnimationKeyframeInterval;
using protocol::kAnimationPacketMagic;
using protocol::kAppearanceChunkBytes;
using protocol::kAppearancePacketMagic;
using protocol::kMaximumAnimationBones;
using protocol::kMaximumAnimationFragments;
using protocol::kMaximumAnimationFrameWords;
using protocol::kMaximumAnimationTracks;
using protocol::kMaximumAppearanceBytes;
using protocol::kPacketMagic;
using protocol::kProtocolVersion;

enum class NetworkQualityPreset : std::int32_t {
  kAuto = 0,
  kBandwidthSaver = 1,
  kBalanced = 2,
  kHighFidelity = 3,
  kCustom = 4,
};

struct NetworkTuning {
  std::int32_t pose_rate = 60;
  std::int32_t animation_rate = 60;
  std::int32_t interpolation_ms = 50;
  float relevance_radius = 80.0f;
  float attachment_radius = 35.0f;
  std::int32_t relevance_players = 12;
  std::int32_t far_presence_rate = 2;
  NetworkQualityPreset selected = NetworkQualityPreset::kBalanced;
  NetworkQualityPreset effective = NetworkQualityPreset::kBalanced;
};

NetworkTuning ResolveNetworkTuning(std::size_t participant_count) {
  const auto selected = static_cast<NetworkQualityPreset>(
      std::clamp(
          REXCVAR_GET(skate3_multiplayer_quality_preset),
          static_cast<std::int32_t>(NetworkQualityPreset::kAuto),
          static_cast<std::int32_t>(NetworkQualityPreset::kCustom)));
  NetworkQualityPreset effective = selected;
  if (effective == NetworkQualityPreset::kAuto) {
    if (participant_count <= 4) {
      effective = NetworkQualityPreset::kHighFidelity;
    } else if (participant_count <= 12) {
      effective = NetworkQualityPreset::kBalanced;
    } else {
      effective = NetworkQualityPreset::kBandwidthSaver;
    }
  }

  NetworkTuning tuning;
  tuning.selected = selected;
  tuning.effective = effective;
  switch (effective) {
    case NetworkQualityPreset::kBandwidthSaver:
      tuning.pose_rate = 30;
      tuning.animation_rate = 10;
      tuning.interpolation_ms = 100;
      tuning.relevance_radius = 50.0f;
      tuning.attachment_radius = 20.0f;
      tuning.relevance_players = 6;
      tuning.far_presence_rate = 1;
      break;
    case NetworkQualityPreset::kHighFidelity:
      tuning.pose_rate = 90;
      tuning.animation_rate = 60;
      tuning.interpolation_ms = 35;
      tuning.relevance_radius = 120.0f;
      tuning.attachment_radius = 50.0f;
      tuning.relevance_players = 16;
      tuning.far_presence_rate = 3;
      break;
    case NetworkQualityPreset::kCustom:
      tuning.pose_rate = std::clamp(
          REXCVAR_GET(skate3_multiplayer_local_send_rate), 10, 120);
      tuning.animation_rate = std::clamp(
          REXCVAR_GET(skate3_multiplayer_local_animation_rate), 10, 60);
      tuning.interpolation_ms = std::clamp(
          REXCVAR_GET(skate3_multiplayer_local_interpolation_ms), 0, 250);
      tuning.relevance_radius = static_cast<float>(std::clamp(
          REXCVAR_GET(skate3_multiplayer_relevance_radius), 5.0, 1000.0));
      tuning.attachment_radius = static_cast<float>(std::clamp(
          REXCVAR_GET(skate3_multiplayer_attachment_radius), 5.0, 250.0));
      tuning.relevance_players = std::clamp(
          REXCVAR_GET(skate3_multiplayer_relevance_players), 1, 32);
      tuning.far_presence_rate = std::clamp(
          REXCVAR_GET(skate3_multiplayer_far_presence_rate), 1, 10);
      break;
    case NetworkQualityPreset::kAuto:
      // Auto is resolved to one of the concrete presets above.
      break;
    case NetworkQualityPreset::kBalanced:
    default:
      // These defaults deliberately preserve the settings used by the
      // visually verified multiplayer build.
      break;
  }
  return tuning;
}

const char* NetworkQualityName(const NetworkTuning& tuning) {
  if (tuning.selected == NetworkQualityPreset::kAuto) {
    switch (tuning.effective) {
      case NetworkQualityPreset::kBandwidthSaver:
        return "Auto/Bandwidth Saver";
      case NetworkQualityPreset::kHighFidelity:
        return "Auto/High Fidelity";
      case NetworkQualityPreset::kBalanced:
      default:
        return "Auto/Balanced";
    }
  }
  switch (tuning.effective) {
    case NetworkQualityPreset::kBandwidthSaver:
      return "Bandwidth Saver";
    case NetworkQualityPreset::kHighFidelity:
      return "High Fidelity";
    case NetworkQualityPreset::kCustom:
      return "Custom";
    case NetworkQualityPreset::kBalanced:
    default:
      return "Balanced";
  }
}

constexpr auto kRemoteTimeout = std::chrono::milliseconds(1500);
constexpr std::size_t kMaximumBufferedSamples = 16;
constexpr std::size_t kMaximumBufferedAnimationSamples = 8;
// Animation matrices stay inside a compact character-relative range. Fixed
// point gives substantially more stable per-frame values than half floats at
// the same 16-bit wire size, avoiding pose-dependent mantissa precision.
constexpr float kAnimationTranslationScale = 4096.0f;
constexpr float kAnimationBasisScale = 8192.0f;
constexpr float kAnimationQuaternionScale = 32767.0f;

struct ReceivedSample {
  Clock::time_point received_at{};
  std::uint64_t sender_time_us = 0;
  RemotePose pose{};
  std::uint32_t sequence = 0;
};

struct ReceivedAnimationSample {
  Clock::time_point received_at{};
  AnimationPose pose{};
};

struct QuantizedAnimationTrack {
  std::uint32_t mesh_key = 0;
  std::uint16_t bone_count = 0;
  AnimationTrackEncoding encoding =
      AnimationTrackEncoding::kAffineRows;
  std::vector<std::uint16_t> words;
};

struct QuantizedAnimationFrame {
  std::uint32_t sequence = 0;
  std::vector<QuantizedAnimationTrack> tracks;
};

struct AnimationAssembly {
  Clock::time_point received_at{};
  std::uint32_t session = 0;
  std::uint32_t sequence = 0;
  std::uint64_t sender_time_us = 0;
  std::uint16_t root_bone = 0xFFFFu;
  float root_position[3] = {};
  std::uint16_t fragment_count = 0;
  std::uint16_t total_words = 0;
  std::uint32_t received_fragments = 0;
  std::vector<std::uint16_t> words;
};

struct AppearanceAssembly {
  std::uint64_t identity = 0;
  std::uint32_t total_bytes = 0;
  std::uint16_t chunk_count = 0;
  std::uint16_t received_chunks = 0;
  Clock::time_point last_update{};
  std::vector<std::uint8_t> bytes;
  std::vector<bool> received;
};

struct RemotePeerState {
  std::uint32_t session = 0;
  Clock::time_point last_packet_at{};
  std::int64_t clock_offset_us = 0;
  std::int64_t minimum_clock_offset_us =
      std::numeric_limits<std::int64_t>::max();
  bool clock_offset_valid = false;
  std::uint32_t clock_rebase_count = 0;
  bool announced = false;
  std::int64_t animation_period_us = 50000;
  std::int64_t animation_jitter_us = 0;
  std::uint64_t last_animation_sender_time_us = 0;
  std::uint64_t last_animation_arrival_time_us = 0;
  std::uint32_t last_animation_sequence = 0;
  std::deque<ReceivedSample> samples;
  std::deque<ReceivedAnimationSample> animation_samples;
  AnimationAssembly animation_assembly;
  QuantizedAnimationFrame animation_keyframe;
  AppearanceAssembly appearance_assembly;
  AppearanceBlob appearance;
};

struct OutboundAppearanceState {
  std::uint64_t identity = 0;
  std::uint16_t next_chunk = 0;
  std::uint8_t completed_passes = 0;
  Clock::time_point retry_after{};
};

std::int64_t PresentationDelayMicroseconds(
    const RemotePeerState& peer, std::int32_t interpolation_ms) {
  const std::int64_t configured_delay_us =
      std::int64_t{std::clamp(interpolation_ms, 0, 250)} * 1000;
  if (peer.animation_samples.empty()) {
    return configured_delay_us;
  }
  const std::int64_t stable_period_us =
      std::clamp<std::int64_t>(
          peer.animation_period_us, 8000, 150000);
  const std::int64_t stable_jitter_us =
      std::clamp<std::int64_t>(
          peer.animation_jitter_us, 0, 50000);
  // Root and skeleton must be sampled from one sender-time point. Presenting
  // the higher-rate root 50 ms ahead of the skeleton makes a clone oscillate
  // between two moments even when both individual streams are smooth.
  return std::clamp<std::int64_t>(
      std::max(
          configured_delay_us,
          stable_period_us * 2 + stable_jitter_us * 3),
      0, 250000);
}

#if defined(_WIN32)
struct PacketEndpoint {
  bool steam = false;
  sockaddr_in udp{};
  std::uint64_t steam_id = 0;
};

struct HostPeer {
  PacketEndpoint endpoint{};
  std::uint32_t session = 0;
  Clock::time_point last_seen{};
  float position[3] = {};
  bool position_valid = false;
};
#endif

struct TelemetrySnapshot {
  bool enabled = false;
  bool socket_ready = false;
  bool remote_visible = false;
  std::int32_t role = 0;
  std::uint32_t session = 0;
  std::uint32_t sent_sequence = 0;
  std::uint32_t received_sequence = 0;
  std::uint64_t sent_packets = 0;
  std::uint64_t received_packets = 0;
  std::uint64_t sent_bytes = 0;
  std::uint64_t received_bytes = 0;
  std::uint64_t sent_root_packets = 0;
  std::uint64_t sent_root_bytes = 0;
  std::uint64_t sent_animation_fragments = 0;
  std::uint64_t sent_animation_bytes = 0;
  std::uint64_t sent_appearance_chunks = 0;
  std::uint64_t sent_appearance_bytes = 0;
  std::uint64_t received_root_packets = 0;
  std::uint64_t received_root_bytes = 0;
  std::uint64_t received_animation_fragments = 0;
  std::uint64_t received_animation_bytes = 0;
  std::uint64_t received_appearance_chunks = 0;
  std::uint64_t received_appearance_bytes = 0;
  std::uint64_t rejected_packets = 0;
  std::uint64_t socket_failures = 0;
  std::uint64_t remote_age_ms = 0;
  std::uint64_t sent_animation_frames = 0;
  std::uint64_t received_animation_frames = 0;
  std::uint32_t remote_animation_bones = 0;
  std::uint32_t known_peers = 0;
  std::uint32_t visible_players = 0;
  std::uint64_t relayed_packets = 0;
  std::uint64_t relevance_drops = 0;
  std::uint64_t far_presence_packets = 0;
  std::uint64_t outbound_peer_resets = 0;
  std::uint64_t appearance_assembly_timeouts = 0;
  std::uint64_t appearance_budget_rejections = 0;
  std::uint64_t incomplete_appearance_bytes = 0;
  std::uint64_t animation_present_interpolated = 0;
  std::uint64_t animation_present_held_latest = 0;
  std::uint64_t animation_present_held_oldest = 0;
  std::int64_t animation_period_us = 0;
  std::int64_t animation_jitter_us = 0;
  std::uint32_t animation_buffered_samples = 0;
  float remote_position[3] = {};
};

float Dot(const float left[3], const float right[3]) {
  return left[0] * right[0] + left[1] * right[1] +
         left[2] * right[2];
}

void Cross(const float left[3], const float right[3], float out[3]) {
  out[0] = left[1] * right[2] - left[2] * right[1];
  out[1] = left[2] * right[0] - left[0] * right[2];
  out[2] = left[0] * right[1] - left[1] * right[0];
}

bool Normalize(float value[3]) {
  const float length_squared = Dot(value, value);
  if (!std::isfinite(length_squared) || length_squared < 1.0e-8f) {
    return false;
  }
  const float inverse_length = 1.0f / std::sqrt(length_squared);
  value[0] *= inverse_length;
  value[1] *= inverse_length;
  value[2] *= inverse_length;
  return true;
}

bool Finite3(const float value[3]) {
  return std::isfinite(value[0]) && std::isfinite(value[1]) &&
         std::isfinite(value[2]);
}

struct Quaternion {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float w = 1.0f;
};

bool NormalizeQuaternion(Quaternion& value) {
  const float length_squared =
      value.x * value.x + value.y * value.y +
      value.z * value.z + value.w * value.w;
  if (!std::isfinite(length_squared) ||
      length_squared < 1.0e-10f) {
    return false;
  }
  const float inverse_length =
      1.0f / std::sqrt(length_squared);
  value.x *= inverse_length;
  value.y *= inverse_length;
  value.z *= inverse_length;
  value.w *= inverse_length;
  return true;
}

bool QuaternionFromRotation(
    const float matrix[9], Quaternion& out) {
  for (std::size_t index = 0; index < 9; ++index) {
    if (!std::isfinite(matrix[index])) {
      return false;
    }
  }
  const float trace =
      matrix[0] + matrix[4] + matrix[8];
  if (trace > 0.0f) {
    const float scale =
        std::sqrt(std::max(trace + 1.0f, 0.0f)) * 2.0f;
    if (scale < 1.0e-6f) {
      return false;
    }
    out.w = 0.25f * scale;
    out.x = (matrix[7] - matrix[5]) / scale;
    out.y = (matrix[2] - matrix[6]) / scale;
    out.z = (matrix[3] - matrix[1]) / scale;
  } else if (matrix[0] > matrix[4] &&
             matrix[0] > matrix[8]) {
    const float scale = std::sqrt(std::max(
                            1.0f + matrix[0] -
                                matrix[4] - matrix[8],
                            0.0f)) *
                        2.0f;
    if (scale < 1.0e-6f) {
      return false;
    }
    out.w = (matrix[7] - matrix[5]) / scale;
    out.x = 0.25f * scale;
    out.y = (matrix[1] + matrix[3]) / scale;
    out.z = (matrix[2] + matrix[6]) / scale;
  } else if (matrix[4] > matrix[8]) {
    const float scale = std::sqrt(std::max(
                            1.0f + matrix[4] -
                                matrix[0] - matrix[8],
                            0.0f)) *
                        2.0f;
    if (scale < 1.0e-6f) {
      return false;
    }
    out.w = (matrix[2] - matrix[6]) / scale;
    out.x = (matrix[1] + matrix[3]) / scale;
    out.y = 0.25f * scale;
    out.z = (matrix[5] + matrix[7]) / scale;
  } else {
    const float scale = std::sqrt(std::max(
                            1.0f + matrix[8] -
                                matrix[0] - matrix[4],
                            0.0f)) *
                        2.0f;
    if (scale < 1.0e-6f) {
      return false;
    }
    out.w = (matrix[3] - matrix[1]) / scale;
    out.x = (matrix[2] + matrix[6]) / scale;
    out.y = (matrix[5] + matrix[7]) / scale;
    out.z = 0.25f * scale;
  }
  return NormalizeQuaternion(out);
}

void RotationFromQuaternion(
    const Quaternion& value, float out[9]) {
  const float xx = value.x * value.x;
  const float yy = value.y * value.y;
  const float zz = value.z * value.z;
  const float xy = value.x * value.y;
  const float xz = value.x * value.z;
  const float yz = value.y * value.z;
  const float wx = value.w * value.x;
  const float wy = value.w * value.y;
  const float wz = value.w * value.z;
  out[0] = 1.0f - 2.0f * (yy + zz);
  out[1] = 2.0f * (xy - wz);
  out[2] = 2.0f * (xz + wy);
  out[3] = 2.0f * (xy + wz);
  out[4] = 1.0f - 2.0f * (xx + zz);
  out[5] = 2.0f * (yz - wx);
  out[6] = 2.0f * (xz - wy);
  out[7] = 2.0f * (yz + wx);
  out[8] = 1.0f - 2.0f * (xx + yy);
}

Quaternion NlerpQuaternion(
    const Quaternion& first, const Quaternion& second,
    float amount) {
  Quaternion aligned = second;
  const float dot =
      first.x * second.x + first.y * second.y +
      first.z * second.z + first.w * second.w;
  if (dot < 0.0f) {
    aligned.x = -aligned.x;
    aligned.y = -aligned.y;
    aligned.z = -aligned.z;
    aligned.w = -aligned.w;
  }
  Quaternion result{
      first.x + (aligned.x - first.x) * amount,
      first.y + (aligned.y - first.y) * amount,
      first.z + (aligned.z - first.z) * amount,
      first.w + (aligned.w - first.w) * amount};
  if (!NormalizeQuaternion(result)) {
    return aligned;
  }
  return result;
}

bool DecomposeAffineRotationScale(
    const float matrix[12], Quaternion& rotation,
    float scale[3]) {
  float x[3] = {matrix[0], matrix[4], matrix[8]};
  float y[3] = {matrix[1], matrix[5], matrix[9]};
  const float original_z[3] = {
      matrix[2], matrix[6], matrix[10]};
  scale[0] = std::sqrt(Dot(x, x));
  if (!std::isfinite(scale[0]) || scale[0] < 1.0e-6f ||
      !Normalize(x)) {
    return false;
  }
  const float xy = Dot(y, x);
  for (std::size_t component = 0; component < 3; ++component) {
    y[component] -= x[component] * xy;
  }
  scale[1] = std::sqrt(Dot(y, y));
  if (!std::isfinite(scale[1]) || scale[1] < 1.0e-6f ||
      !Normalize(y)) {
    return false;
  }
  float z[3];
  Cross(x, y, z);
  if (!Normalize(z)) {
    return false;
  }
  scale[2] = Dot(original_z, z);
  if (!std::isfinite(scale[2]) ||
      std::fabs(scale[2]) < 1.0e-6f) {
    return false;
  }
  const float rotation_matrix[9] = {
      x[0], y[0], z[0],
      x[1], y[1], z[1],
      x[2], y[2], z[2]};
  return QuaternionFromRotation(rotation_matrix, rotation);
}

std::uint16_t QuantizeSigned(float value, float scale) {
  return static_cast<std::uint16_t>(
      static_cast<std::int16_t>(
          std::clamp(std::lround(value * scale), -32768l, 32767l)));
}

float DequantizeSigned(std::uint16_t value, float scale) {
  return static_cast<float>(
             static_cast<std::int16_t>(value)) /
         scale;
}

std::size_t TrackWordStride(AnimationTrackEncoding encoding) {
  switch (encoding) {
    case AnimationTrackEncoding::kRigidQuaternion:
      return 7u;
    case AnimationTrackEncoding::kAffineRowsWideTranslation:
      return 15u;
    case AnimationTrackEncoding::kRigidQuaternionWideTranslation:
      return 10u;
    case AnimationTrackEncoding::kAffineRows:
    default:
      return 12u;
  }
}

bool RigidTrackEncoding(AnimationTrackEncoding encoding) {
  return encoding == AnimationTrackEncoding::kRigidQuaternion ||
         encoding ==
             AnimationTrackEncoding::kRigidQuaternionWideTranslation;
}

bool WideTranslationTrackEncoding(AnimationTrackEncoding encoding) {
  return encoding ==
             AnimationTrackEncoding::kAffineRowsWideTranslation ||
         encoding ==
             AnimationTrackEncoding::kRigidQuaternionWideTranslation;
}

void AppendI32(std::vector<std::uint16_t>& words,
               std::int32_t value) {
  const std::uint32_t bits =
      static_cast<std::uint32_t>(value);
  words.push_back(static_cast<std::uint16_t>(bits));
  words.push_back(static_cast<std::uint16_t>(bits >> 16));
}

std::int32_t QuantizeSignedWide(float value, float scale) {
  const double scaled =
      static_cast<double>(value) * static_cast<double>(scale);
  return static_cast<std::int32_t>(std::clamp(
      std::llround(scaled),
      static_cast<long long>(
          std::numeric_limits<std::int32_t>::min()),
      static_cast<long long>(
          std::numeric_limits<std::int32_t>::max())));
}

float DequantizeSignedWide(const std::uint16_t* words, float scale) {
  const std::uint32_t bits =
      static_cast<std::uint32_t>(words[0]) |
      (static_cast<std::uint32_t>(words[1]) << 16);
  return static_cast<float>(
             static_cast<std::int32_t>(bits)) /
         scale;
}

bool QuantizeAnimationTrack(
    const AnimationTrack& source, const float root_position[3],
    QuantizedAnimationTrack& output) {
  if (source.mesh_key == 0 || source.bone_rows.size() < 12 ||
      source.bone_rows.size() % 12 != 0 ||
      !Finite3(root_position)) {
    return false;
  }
  output = {};
  output.mesh_key = source.mesh_key;
  output.bone_count = static_cast<std::uint16_t>(
      std::min<std::size_t>(
          source.bone_rows.size() / 12,
          kMaximumAnimationBones));
  bool rigid = true;
  bool wide_translation = false;
  float maximum_relative_translation = 0.0f;
  std::vector<Quaternion> rotations(output.bone_count);
  for (std::size_t bone = 0; bone < output.bone_count; ++bone) {
    const float* rows =
        source.bone_rows.data() + bone * 12;
    for (std::size_t axis = 0; axis < 3; ++axis) {
      const float relative_translation =
          rows[axis * 4 + 3] - root_position[axis];
      if (!std::isfinite(relative_translation)) {
        return false;
      }
      maximum_relative_translation = std::max(
          maximum_relative_translation,
          std::fabs(relative_translation));
      wide_translation =
          wide_translation ||
          std::fabs(relative_translation) *
                  kAnimationTranslationScale >
              static_cast<float>(
                  std::numeric_limits<std::int16_t>::max());
    }
    float scale[3];
    if (!DecomposeAffineRotationScale(
            rows, rotations[bone], scale) ||
        std::fabs(scale[0] - 1.0f) > 0.01f ||
        std::fabs(scale[1] - 1.0f) > 0.01f ||
        std::fabs(scale[2] - 1.0f) > 0.01f) {
      rigid = false;
      break;
    }
  }
  output.encoding =
      rigid
          ? (wide_translation
                 ? AnimationTrackEncoding::
                       kRigidQuaternionWideTranslation
                 : AnimationTrackEncoding::kRigidQuaternion)
          : (wide_translation
                 ? AnimationTrackEncoding::
                       kAffineRowsWideTranslation
                 : AnimationTrackEncoding::kAffineRows);
  if (wide_translation) {
    static std::mutex s_wide_track_log_mutex;
    static std::unordered_set<std::uint32_t>
        s_wide_track_logs;
    std::lock_guard<std::mutex> lock(
        s_wide_track_log_mutex);
    if (s_wide_track_logs.insert(source.mesh_key).second) {
      REXLOG_INFO(
          "multiplayer-animation-wide-track: key={:08X} "
          "bones={} max_relative_translation={:.3f}",
          source.mesh_key, output.bone_count,
          maximum_relative_translation);
    }
  }
  output.words.reserve(
      std::size_t(output.bone_count) *
      TrackWordStride(output.encoding));
  for (std::size_t bone = 0; bone < output.bone_count; ++bone) {
    const float* rows = source.bone_rows.data() + bone * 12;
    if (rigid) {
      const Quaternion& rotation = rotations[bone];
      output.words.push_back(
          QuantizeSigned(rotation.x, kAnimationQuaternionScale));
      output.words.push_back(
          QuantizeSigned(rotation.y, kAnimationQuaternionScale));
      output.words.push_back(
          QuantizeSigned(rotation.z, kAnimationQuaternionScale));
      output.words.push_back(
          QuantizeSigned(rotation.w, kAnimationQuaternionScale));
      for (std::size_t axis = 0; axis < 3; ++axis) {
        const float relative_translation =
            rows[axis * 4 + 3] - root_position[axis];
        if (wide_translation) {
          AppendI32(
              output.words,
              QuantizeSignedWide(
                  relative_translation,
                  kAnimationTranslationScale));
        } else {
          output.words.push_back(QuantizeSigned(
              relative_translation,
              kAnimationTranslationScale));
        }
      }
      continue;
    }
    for (std::size_t component = 0; component < 12; ++component) {
      if (!std::isfinite(rows[component])) {
        return false;
      }
      if (component == 3 || component == 7 || component == 11) {
        const float relative_translation =
            rows[component] -
            root_position[(component - 3) / 4];
        if (wide_translation) {
          AppendI32(
              output.words,
              QuantizeSignedWide(
                  relative_translation,
                  kAnimationTranslationScale));
        } else {
          output.words.push_back(QuantizeSigned(
              relative_translation,
              kAnimationTranslationScale));
        }
      } else {
        output.words.push_back(
            QuantizeSigned(rows[component], kAnimationBasisScale));
      }
    }
  }
  return true;
}

void AppendU32(std::vector<std::uint16_t>& words,
               std::uint32_t value) {
  words.push_back(static_cast<std::uint16_t>(value));
  words.push_back(static_cast<std::uint16_t>(value >> 16));
}

bool ReadU32(const std::vector<std::uint16_t>& words,
             std::size_t& cursor, std::uint32_t& value) {
  if (cursor + 2 > words.size()) {
    return false;
  }
  value =
      static_cast<std::uint32_t>(words[cursor]) |
      (static_cast<std::uint32_t>(words[cursor + 1]) << 16);
  cursor += 2;
  return true;
}

bool SameTrackLayout(const QuantizedAnimationTrack& left,
                     const QuantizedAnimationTrack& right) {
  return left.mesh_key == right.mesh_key &&
         left.bone_count == right.bone_count &&
         left.encoding == right.encoding &&
         left.words.size() == right.words.size();
}

bool BuildAnimationFrameWords(
    const AnimationPose& pose,
    const std::vector<const AnimationTrack*>& tracks,
    std::uint32_t sequence,
    QuantizedAnimationFrame& keyframe,
    std::vector<std::uint16_t>& words) {
  QuantizedAnimationFrame current;
  current.sequence = sequence;
  current.tracks.reserve(tracks.size());
  for (const AnimationTrack* track : tracks) {
    QuantizedAnimationTrack quantized;
    if (track != nullptr &&
        QuantizeAnimationTrack(
            *track, pose.root_position, quantized)) {
      current.tracks.push_back(std::move(quantized));
    }
  }
  if (current.tracks.empty() ||
      current.tracks.size() > kMaximumAnimationTracks) {
    return false;
  }
  bool keyframe_required =
      keyframe.tracks.size() != current.tracks.size() ||
      keyframe.sequence == 0 ||
      sequence - keyframe.sequence >= kAnimationKeyframeInterval;
  if (!keyframe_required) {
    for (std::size_t index = 0;
         index < current.tracks.size(); ++index) {
      if (!SameTrackLayout(
              current.tracks[index], keyframe.tracks[index])) {
        keyframe_required = true;
        break;
      }
    }
  }

  words.clear();
  words.reserve(4096);
  words.push_back(keyframe_required ? 1u : 0u);
  words.push_back(
      static_cast<std::uint16_t>(current.tracks.size()));
  AppendU32(
      words,
      keyframe_required ? sequence : keyframe.sequence);
  for (std::size_t track_index = 0;
       track_index < current.tracks.size(); ++track_index) {
    const QuantizedAnimationTrack& track =
        current.tracks[track_index];
    AppendU32(words, track.mesh_key);
    words.push_back(track.bone_count);
    words.push_back(
        static_cast<std::uint16_t>(track.encoding));
    const std::size_t stride =
        TrackWordStride(track.encoding);
    if (keyframe_required) {
      words.push_back(0);
      words.insert(
          words.end(), track.words.begin(), track.words.end());
      continue;
    }
    const QuantizedAnimationTrack& base =
        keyframe.tracks[track_index];
    const std::size_t mask_words =
        (std::size_t(track.bone_count) + 15) / 16;
    words.push_back(
        static_cast<std::uint16_t>(mask_words));
    const std::size_t mask_start = words.size();
    words.resize(words.size() + mask_words, 0);
    for (std::size_t bone = 0; bone < track.bone_count; ++bone) {
      const std::size_t offset = bone * stride;
      const bool changed =
          !std::equal(
              track.words.begin() + offset,
              track.words.begin() + offset + stride,
              base.words.begin() + offset);
      if (!changed) {
        continue;
      }
      words[mask_start + bone / 16] |=
          static_cast<std::uint16_t>(1u << (bone % 16));
      words.insert(
          words.end(),
          track.words.begin() + offset,
          track.words.begin() + offset + stride);
    }
  }
  if (words.size() > kMaximumAnimationFrameWords) {
    return false;
  }
  if (keyframe_required) {
    keyframe = std::move(current);
  }
  return true;
}

bool DecodeAnimationFrameWords(
    const std::vector<std::uint16_t>& words,
    std::uint32_t sequence, const float root_position[3],
    QuantizedAnimationFrame& keyframe,
    std::vector<AnimationTrack>& output,
    std::uint32_t& total_bones) {
  if (words.size() < 4 || !Finite3(root_position)) {
    return false;
  }
  std::size_t cursor = 0;
  const bool is_keyframe = (words[cursor++] & 1u) != 0;
  const std::size_t track_count = words[cursor++];
  std::uint32_t base_sequence = 0;
  if (!ReadU32(words, cursor, base_sequence) ||
      track_count == 0 ||
      track_count > kMaximumAnimationTracks ||
      (is_keyframe && base_sequence != sequence) ||
      (!is_keyframe &&
       (keyframe.sequence != base_sequence ||
        keyframe.tracks.size() != track_count))) {
    return false;
  }
  QuantizedAnimationFrame current;
  current.sequence = sequence;
  current.tracks.reserve(track_count);
  for (std::size_t track_index = 0;
       track_index < track_count; ++track_index) {
    QuantizedAnimationTrack track;
    std::uint32_t mesh_key = 0;
    if (!ReadU32(words, cursor, mesh_key) ||
        cursor + 3 > words.size()) {
      return false;
    }
    track.mesh_key = mesh_key;
    track.bone_count = words[cursor++];
    track.encoding =
        static_cast<AnimationTrackEncoding>(words[cursor++]);
    const std::size_t mask_words = words[cursor++];
    if (track.mesh_key == 0 || track.bone_count == 0 ||
        track.bone_count > kMaximumAnimationBones ||
        (track.encoding != AnimationTrackEncoding::kAffineRows &&
         track.encoding !=
             AnimationTrackEncoding::kRigidQuaternion &&
         track.encoding !=
             AnimationTrackEncoding::kAffineRowsWideTranslation &&
         track.encoding !=
             AnimationTrackEncoding::
                 kRigidQuaternionWideTranslation)) {
      return false;
    }
    const std::size_t stride =
        TrackWordStride(track.encoding);
    const std::size_t track_words =
        std::size_t(track.bone_count) * stride;
    if (is_keyframe) {
      if (mask_words != 0 ||
          cursor + track_words > words.size()) {
        return false;
      }
      track.words.assign(
          words.begin() + cursor,
          words.begin() + cursor + track_words);
      cursor += track_words;
    } else {
      const QuantizedAnimationTrack& base =
          keyframe.tracks[track_index];
      const std::size_t expected_mask_words =
          (std::size_t(track.bone_count) + 15) / 16;
      if (track.mesh_key != base.mesh_key ||
          track.bone_count != base.bone_count ||
          track.encoding != base.encoding ||
          base.words.size() != track_words ||
          mask_words != expected_mask_words ||
          cursor + mask_words > words.size()) {
        return false;
      }
      track.words = base.words;
      const std::size_t mask_start = cursor;
      cursor += mask_words;
      for (std::size_t bone = 0;
           bone < track.bone_count; ++bone) {
        if ((words[mask_start + bone / 16] &
             static_cast<std::uint16_t>(
                 1u << (bone % 16))) == 0) {
          continue;
        }
        if (cursor + stride > words.size()) {
          return false;
        }
        std::copy_n(
            words.begin() + cursor, stride,
            track.words.begin() + bone * stride);
        cursor += stride;
      }
    }
    current.tracks.push_back(std::move(track));
  }
  if (cursor != words.size()) {
    return false;
  }
  if (is_keyframe) {
    keyframe = current;
  }

  output.clear();
  output.reserve(current.tracks.size());
  total_bones = 0;
  for (const QuantizedAnimationTrack& track : current.tracks) {
    AnimationTrack decoded;
    decoded.mesh_key = track.mesh_key;
    decoded.bone_rows.resize(
        std::size_t(track.bone_count) * 12);
    const std::size_t stride =
        TrackWordStride(track.encoding);
    for (std::size_t bone = 0; bone < track.bone_count; ++bone) {
      const std::uint16_t* source =
          track.words.data() + bone * stride;
      float* rows =
          decoded.bone_rows.data() + bone * 12;
      if (RigidTrackEncoding(track.encoding)) {
        Quaternion rotation{
            DequantizeSigned(
                source[0], kAnimationQuaternionScale),
            DequantizeSigned(
                source[1], kAnimationQuaternionScale),
            DequantizeSigned(
                source[2], kAnimationQuaternionScale),
            DequantizeSigned(
                source[3], kAnimationQuaternionScale)};
        if (!NormalizeQuaternion(rotation)) {
          return false;
        }
        float rotation_matrix[9];
        RotationFromQuaternion(rotation, rotation_matrix);
        for (std::size_t row = 0; row < 3; ++row) {
          for (std::size_t column = 0;
               column < 3; ++column) {
            rows[row * 4 + column] =
                rotation_matrix[row * 3 + column];
          }
          const std::size_t translation_offset =
              WideTranslationTrackEncoding(track.encoding)
                  ? 4 + row * 2
                  : 4 + row;
          rows[row * 4 + 3] =
              root_position[row] +
              (WideTranslationTrackEncoding(track.encoding)
                   ? DequantizeSignedWide(
                         source + translation_offset,
                         kAnimationTranslationScale)
                   : DequantizeSigned(
                         source[translation_offset],
                         kAnimationTranslationScale));
        }
      } else {
        std::size_t source_component = 0;
        for (std::size_t component = 0;
             component < 12; ++component) {
          if (component == 3 || component == 7 ||
              component == 11) {
            rows[component] =
                root_position[(component - 3) / 4] +
                (WideTranslationTrackEncoding(track.encoding)
                     ? DequantizeSignedWide(
                           source + source_component,
                           kAnimationTranslationScale)
                     : DequantizeSigned(
                           source[source_component],
                           kAnimationTranslationScale));
            source_component +=
                WideTranslationTrackEncoding(track.encoding) ? 2 : 1;
          } else {
            rows[component] =
                DequantizeSigned(
                    source[source_component],
                    kAnimationBasisScale);
            ++source_component;
          }
        }
      }
    }
    total_bones += track.bone_count;
    output.push_back(std::move(decoded));
  }
  return true;
}

void InterpolateAffine(
    const float first[12], const float second[12],
    float amount, float out[12]) {
  Quaternion first_rotation;
  Quaternion second_rotation;
  float first_scale[3];
  float second_scale[3];
  if (!DecomposeAffineRotationScale(
          first, first_rotation, first_scale) ||
      !DecomposeAffineRotationScale(
          second, second_rotation, second_scale)) {
    for (std::size_t component = 0;
         component < 12; ++component) {
      out[component] =
          first[component] +
          (second[component] - first[component]) * amount;
    }
    return;
  }
  const Quaternion rotation =
      NlerpQuaternion(first_rotation, second_rotation, amount);
  float rotation_matrix[9];
  RotationFromQuaternion(rotation, rotation_matrix);
  for (std::size_t column = 0; column < 3; ++column) {
    const float interpolated_scale =
        first_scale[column] +
        (second_scale[column] - first_scale[column]) * amount;
    for (std::size_t row = 0; row < 3; ++row) {
      out[row * 4 + column] =
          rotation_matrix[row * 3 + column] *
          interpolated_scale;
    }
  }
  out[3] = first[3] + (second[3] - first[3]) * amount;
  out[7] = first[7] + (second[7] - first[7]) * amount;
  out[11] = first[11] + (second[11] - first[11]) * amount;
}

void InterpolateAttachmentAffine(
    const float first[12], const float second[12],
    float amount, float out[12]) {
  Quaternion first_rotation;
  Quaternion second_rotation;
  float first_scale[3];
  float second_scale[3];
  if (!DecomposeAffineRotationScale(
          first, first_rotation, first_scale) ||
      !DecomposeAffineRotationScale(
          second, second_rotation, second_scale)) {
    for (std::size_t component = 0;
         component < 12; ++component) {
      out[component] =
          first[component] +
          (second[component] - first[component]) * amount;
    }
    return;
  }
  // A skinning affine's translation is not an independent bone position.
  // It includes the inverse-bind pivot compensation (p - R*p). Slerping R
  // while linearly interpolating t therefore moves a rigidly weighted
  // attachment away from its pivot; the error is subtle on a hat and huge
  // on fast-spinning skateboard wheels. Interpolate the rigid part on SE(3)
  // instead, so rotation and translation follow one screw transform.
  Quaternion aligned_second = second_rotation;
  const float rotation_dot =
      first_rotation.x * second_rotation.x +
      first_rotation.y * second_rotation.y +
      first_rotation.z * second_rotation.z +
      first_rotation.w * second_rotation.w;
  if (rotation_dot < 0.0f) {
    aligned_second.x = -aligned_second.x;
    aligned_second.y = -aligned_second.y;
    aligned_second.z = -aligned_second.z;
    aligned_second.w = -aligned_second.w;
  }
  const auto multiply_quaternion =
      [](const Quaternion& left,
         const Quaternion& right) {
        return Quaternion{
            left.w * right.x + left.x * right.w +
                left.y * right.z - left.z * right.y,
            left.w * right.y - left.x * right.z +
                left.y * right.w + left.z * right.x,
            left.w * right.z + left.x * right.y -
                left.y * right.x + left.z * right.w,
            left.w * right.w - left.x * right.x -
                left.y * right.y - left.z * right.z};
      };
  const Quaternion inverse_first{
      -first_rotation.x, -first_rotation.y,
      -first_rotation.z, first_rotation.w};
  Quaternion relative_rotation =
      multiply_quaternion(inverse_first, aligned_second);
  if (!NormalizeQuaternion(relative_rotation)) {
    relative_rotation = {0.0f, 0.0f, 0.0f, 1.0f};
  }
  if (relative_rotation.w < 0.0f) {
    relative_rotation.x = -relative_rotation.x;
    relative_rotation.y = -relative_rotation.y;
    relative_rotation.z = -relative_rotation.z;
    relative_rotation.w = -relative_rotation.w;
  }
  const double relative_w =
      std::clamp<double>(
          relative_rotation.w, -1.0, 1.0);
  const double theta = 2.0 * std::acos(relative_w);
  double phi[3] = {};
  const double half_sine =
      std::sqrt(std::max(
          1.0 - relative_w * relative_w, 0.0));
  if (theta > 1.0e-8 && half_sine > 1.0e-8) {
    const double factor = theta / half_sine;
    phi[0] = double(relative_rotation.x) * factor;
    phi[1] = double(relative_rotation.y) * factor;
    phi[2] = double(relative_rotation.z) * factor;
  }
  const auto skew = [](const double vector[3],
                       double matrix[9]) {
    matrix[0] = 0.0;
    matrix[1] = -vector[2];
    matrix[2] = vector[1];
    matrix[3] = vector[2];
    matrix[4] = 0.0;
    matrix[5] = -vector[0];
    matrix[6] = -vector[1];
    matrix[7] = vector[0];
    matrix[8] = 0.0;
  };
  const auto multiply_matrix3 =
      [](const double left[9], const double right[9],
         double result[9]) {
        for (std::size_t row = 0; row < 3; ++row) {
          for (std::size_t column = 0; column < 3;
               ++column) {
            result[row * 3 + column] = 0.0;
            for (std::size_t inner = 0; inner < 3;
                 ++inner) {
              result[row * 3 + column] +=
                  left[row * 3 + inner] *
                  right[inner * 3 + column];
            }
          }
        }
      };
  double omega[9];
  double omega_squared[9];
  skew(phi, omega);
  multiply_matrix3(omega, omega, omega_squared);
  double inverse_left_jacobian[9] = {
      1.0, 0.0, 0.0,
      0.0, 1.0, 0.0,
      0.0, 0.0, 1.0};
  double inverse_coefficient = 1.0 / 12.0;
  if (theta > 1.0e-5) {
    const double sine = std::sin(theta);
    const double cosine = std::cos(theta);
    inverse_coefficient =
        std::fabs(sine) < 1.0e-8
            ? 1.0 / (theta * theta)
            : 1.0 / (theta * theta) -
                  (1.0 + cosine) /
                      (2.0 * theta * sine);
  }
  for (std::size_t component = 0; component < 9;
       ++component) {
    inverse_left_jacobian[component] +=
        -0.5 * omega[component] +
        inverse_coefficient * omega_squared[component];
  }
  float first_rotation_matrix[9];
  RotationFromQuaternion(
      first_rotation, first_rotation_matrix);
  const double translation_delta[3] = {
      double(second[3]) - first[3],
      double(second[7]) - first[7],
      double(second[11]) - first[11]};
  double relative_translation[3] = {};
  for (std::size_t row = 0; row < 3; ++row) {
    for (std::size_t column = 0; column < 3;
         ++column) {
      relative_translation[row] +=
          double(first_rotation_matrix[column * 3 + row]) *
          translation_delta[column];
    }
  }
  double twist_translation[3] = {};
  for (std::size_t row = 0; row < 3; ++row) {
    for (std::size_t column = 0; column < 3;
         ++column) {
      twist_translation[row] +=
          inverse_left_jacobian[row * 3 + column] *
          relative_translation[column];
    }
  }
  const double interpolated_theta =
      theta * double(amount);
  const double scaled_phi[3] = {
      phi[0] * double(amount),
      phi[1] * double(amount),
      phi[2] * double(amount)};
  double scaled_omega[9];
  double scaled_omega_squared[9];
  skew(scaled_phi, scaled_omega);
  multiply_matrix3(
      scaled_omega, scaled_omega,
      scaled_omega_squared);
  double left_jacobian[9] = {
      1.0, 0.0, 0.0,
      0.0, 1.0, 0.0,
      0.0, 0.0, 1.0};
  double first_coefficient = 0.5;
  double second_coefficient = 1.0 / 6.0;
  if (interpolated_theta > 1.0e-5) {
    first_coefficient =
        (1.0 - std::cos(interpolated_theta)) /
        (interpolated_theta * interpolated_theta);
    second_coefficient =
        (interpolated_theta -
         std::sin(interpolated_theta)) /
        (interpolated_theta * interpolated_theta *
         interpolated_theta);
  }
  for (std::size_t component = 0; component < 9;
       ++component) {
    left_jacobian[component] +=
        first_coefficient * scaled_omega[component] +
        second_coefficient *
            scaled_omega_squared[component];
  }
  const double scaled_twist_translation[3] = {
      twist_translation[0] * double(amount),
      twist_translation[1] * double(amount),
      twist_translation[2] * double(amount)};
  double interpolated_relative_translation[3] = {};
  for (std::size_t row = 0; row < 3; ++row) {
    for (std::size_t column = 0; column < 3;
         ++column) {
      interpolated_relative_translation[row] +=
          left_jacobian[row * 3 + column] *
          scaled_twist_translation[column];
    }
  }
  Quaternion relative_step{
      0.0f, 0.0f, 0.0f, 1.0f};
  if (interpolated_theta > 1.0e-8 &&
      theta > 1.0e-8) {
    const double sine =
        std::sin(interpolated_theta * 0.5);
    const double factor = sine / theta;
    relative_step.x =
        static_cast<float>(phi[0] * factor);
    relative_step.y =
        static_cast<float>(phi[1] * factor);
    relative_step.z =
        static_cast<float>(phi[2] * factor);
    relative_step.w = static_cast<float>(
        std::cos(interpolated_theta * 0.5));
  }
  Quaternion rotation =
      multiply_quaternion(first_rotation, relative_step);
  NormalizeQuaternion(rotation);
  float rotation_matrix[9];
  RotationFromQuaternion(rotation, rotation_matrix);
  for (std::size_t column = 0; column < 3; ++column) {
    const float interpolated_scale =
        first_scale[column] +
        (second_scale[column] - first_scale[column]) * amount;
    for (std::size_t row = 0; row < 3; ++row) {
      out[row * 4 + column] =
          rotation_matrix[row * 3 + column] *
          interpolated_scale;
    }
  }
  for (std::size_t row = 0; row < 3; ++row) {
    double translation =
        first[row * 4 + 3];
    for (std::size_t column = 0; column < 3;
         ++column) {
      translation +=
          double(first_rotation_matrix[row * 3 + column]) *
          interpolated_relative_translation[column];
    }
    out[row * 4 + 3] =
        static_cast<float>(translation);
  }
}

float InterpolateHermite(
    float previous, float first, float second, float next,
    std::uint64_t previous_time, std::uint64_t first_time,
    std::uint64_t second_time, std::uint64_t next_time,
    float amount) {
  const double segment =
      static_cast<double>(second_time - first_time);
  if (!(segment > 0.0)) {
    return second;
  }
  const double first_span =
      static_cast<double>(second_time - previous_time);
  const double second_span =
      static_cast<double>(next_time - first_time);
  const double first_tangent =
      first_span > 0.0
          ? (static_cast<double>(second) -
             static_cast<double>(previous)) *
                segment / first_span
          : static_cast<double>(second) -
                static_cast<double>(first);
  const double second_tangent =
      second_span > 0.0
          ? (static_cast<double>(next) -
             static_cast<double>(first)) *
                segment / second_span
          : static_cast<double>(second) -
                static_cast<double>(first);
  const double t = std::clamp(
      static_cast<double>(amount), 0.0, 1.0);
  const double t2 = t * t;
  const double t3 = t2 * t;
  const double h00 = 2.0 * t3 - 3.0 * t2 + 1.0;
  const double h10 = t3 - 2.0 * t2 + t;
  const double h01 = -2.0 * t3 + 3.0 * t2;
  const double h11 = t3 - t2;
  return static_cast<float>(
      h00 * static_cast<double>(first) +
      h10 * first_tangent +
      h01 * static_cast<double>(second) +
      h11 * second_tangent);
}

void InterpolateAffineHermite(
    const float previous[12], const float first[12],
    const float second[12], const float next[12],
    std::uint64_t previous_time, std::uint64_t first_time,
    std::uint64_t second_time, std::uint64_t next_time,
    float amount, float out[12]) {
  for (std::size_t component = 0; component < 12; ++component) {
    out[component] = InterpolateHermite(
        previous[component], first[component],
        second[component], next[component],
        previous_time, first_time, second_time, next_time,
        amount);
  }
}

std::uint16_t FloatToHalf(float value) {
  const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
  const std::uint32_t sign = (bits >> 16) & 0x8000u;
  const std::uint32_t exponent = (bits >> 23) & 0xffu;
  std::uint32_t mantissa = bits & 0x7fffffu;
  if (exponent == 0xffu) {
    return static_cast<std::uint16_t>(
        sign | (mantissa == 0 ? 0x7c00u : 0x7e00u));
  }
  const int half_exponent = static_cast<int>(exponent) - 127 + 15;
  if (half_exponent >= 31) {
    return static_cast<std::uint16_t>(sign | 0x7c00u);
  }
  if (half_exponent <= 0) {
    if (half_exponent < -10) {
      return static_cast<std::uint16_t>(sign);
    }
    mantissa |= 0x800000u;
    const int shift = 14 - half_exponent;
    std::uint32_t rounded = mantissa >> shift;
    if ((mantissa >> (shift - 1)) & 1u) {
      ++rounded;
    }
    return static_cast<std::uint16_t>(sign | rounded);
  }
  std::uint32_t rounded = mantissa + 0x1000u;
  std::uint32_t encoded_exponent =
      static_cast<std::uint32_t>(half_exponent);
  if (rounded & 0x800000u) {
    rounded = 0;
    ++encoded_exponent;
    if (encoded_exponent >= 31) {
      return static_cast<std::uint16_t>(sign | 0x7c00u);
    }
  }
  return static_cast<std::uint16_t>(
      sign | (encoded_exponent << 10) | (rounded >> 13));
}

float HalfToFloat(std::uint16_t value) {
  const std::uint32_t sign =
      static_cast<std::uint32_t>(value & 0x8000u) << 16;
  std::uint32_t exponent = (value >> 10) & 0x1fu;
  std::uint32_t mantissa = value & 0x03ffu;
  std::uint32_t bits = 0;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {
      int unbiased = -14;
      while ((mantissa & 0x0400u) == 0) {
        mantissa <<= 1;
        --unbiased;
      }
      mantissa &= 0x03ffu;
      bits = sign |
             (static_cast<std::uint32_t>(unbiased + 127) << 23) |
             (mantissa << 13);
    }
  } else if (exponent == 31) {
    bits = sign | 0x7f800000u | (mantissa << 13);
  } else {
    bits = sign | ((exponent - 15 + 127) << 23) |
           (mantissa << 13);
  }
  return std::bit_cast<float>(bits);
}

std::uint32_t HashMapName(const char* map_name) {
  std::uint32_t hash = 2166136261u;
  const std::string_view name =
      map_name == nullptr ? std::string_view{} : std::string_view(map_name);
  for (const unsigned char value : name) {
    hash ^= value;
    hash *= 16777619u;
  }
  return hash;
}

std::uint64_t NowMicroseconds() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          Clock::now().time_since_epoch())
          .count());
}

bool DecodePacketPose(const PosePacket& packet, RemotePose& out) {
  std::memcpy(out.position, packet.position, sizeof(out.position));
  std::memcpy(out.x_axis, packet.x_axis, sizeof(out.x_axis));
  std::memcpy(out.z_axis, packet.z_axis, sizeof(out.z_axis));
  if (!Finite3(out.position) || !Finite3(out.x_axis) ||
      !Finite3(out.z_axis) || !Normalize(out.x_axis) ||
      !Normalize(out.z_axis)) {
    return false;
  }

  // Re-orthogonalize the transmitted board basis. The live guest matrix can
  // accumulate tiny scale/shear errors which should not reach the renderer.
  const float projection = Dot(out.z_axis, out.x_axis);
  for (std::size_t component = 0; component < 3; ++component) {
    out.z_axis[component] -= out.x_axis[component] * projection;
  }
  if (!Normalize(out.z_axis)) {
    return false;
  }
  Cross(out.z_axis, out.x_axis, out.y_axis);
  if (!Normalize(out.y_axis)) {
    return false;
  }
  out.board_state_flags = packet.board_state_flags;
  return true;
}

RemotePose InterpolatePose(const RemotePose& first,
                           const RemotePose& second, float amount) {
  RemotePose result;
  amount = std::clamp(amount, 0.0f, 1.0f);
  for (std::size_t component = 0; component < 3; ++component) {
    result.position[component] =
        first.position[component] +
        (second.position[component] - first.position[component]) * amount;
  }
  const float first_matrix[9] = {
      first.x_axis[0], first.x_axis[1], first.x_axis[2],
      first.y_axis[0], first.y_axis[1], first.y_axis[2],
      first.z_axis[0], first.z_axis[1], first.z_axis[2]};
  const float second_matrix[9] = {
      second.x_axis[0], second.x_axis[1], second.x_axis[2],
      second.y_axis[0], second.y_axis[1], second.y_axis[2],
      second.z_axis[0], second.z_axis[1], second.z_axis[2]};
  Quaternion first_rotation;
  Quaternion second_rotation;
  if (QuaternionFromRotation(first_matrix, first_rotation) &&
      QuaternionFromRotation(second_matrix, second_rotation)) {
    const Quaternion rotation = NlerpQuaternion(
        first_rotation, second_rotation, amount);
    float matrix[9];
    RotationFromQuaternion(rotation, matrix);
    std::copy_n(matrix, 3, result.x_axis);
    std::copy_n(matrix + 3, 3, result.y_axis);
    std::copy_n(matrix + 6, 3, result.z_axis);
  } else {
    for (std::size_t component = 0; component < 3; ++component) {
      result.x_axis[component] =
          first.x_axis[component] +
          (second.x_axis[component] -
           first.x_axis[component]) *
              amount;
      result.z_axis[component] =
          first.z_axis[component] +
          (second.z_axis[component] -
           first.z_axis[component]) *
              amount;
    }
    if (!Normalize(result.x_axis)) {
      std::memcpy(
          result.x_axis, second.x_axis,
          sizeof(result.x_axis));
    }
    const float projection =
        Dot(result.z_axis, result.x_axis);
    for (std::size_t component = 0; component < 3; ++component) {
      result.z_axis[component] -=
          result.x_axis[component] * projection;
    }
    if (!Normalize(result.z_axis)) {
      std::memcpy(
          result.z_axis, second.z_axis,
          sizeof(result.z_axis));
    }
    Cross(result.z_axis, result.x_axis, result.y_axis);
    Normalize(result.y_axis);
  }
  result.board_state_flags = second.board_state_flags;
  return result;
}

RemotePose ExtrapolatePose(
    const RemotePose& previous, const RemotePose& latest,
    float intervals_ahead) {
  RemotePose result;
  for (std::size_t component = 0; component < 3; ++component) {
    result.position[component] =
        latest.position[component] +
        (latest.position[component] -
         previous.position[component]) *
            intervals_ahead;
  }
  const float previous_matrix[9] = {
      previous.x_axis[0], previous.x_axis[1], previous.x_axis[2],
      previous.y_axis[0], previous.y_axis[1], previous.y_axis[2],
      previous.z_axis[0], previous.z_axis[1], previous.z_axis[2]};
  const float latest_matrix[9] = {
      latest.x_axis[0], latest.x_axis[1], latest.x_axis[2],
      latest.y_axis[0], latest.y_axis[1], latest.y_axis[2],
      latest.z_axis[0], latest.z_axis[1], latest.z_axis[2]};
  Quaternion previous_rotation;
  Quaternion latest_rotation;
  if (QuaternionFromRotation(
          previous_matrix, previous_rotation) &&
      QuaternionFromRotation(
          latest_matrix, latest_rotation)) {
    const Quaternion rotation = NlerpQuaternion(
        previous_rotation, latest_rotation,
        1.0f + intervals_ahead);
    float matrix[9];
    RotationFromQuaternion(rotation, matrix);
    std::copy_n(matrix, 3, result.x_axis);
    std::copy_n(matrix + 3, 3, result.y_axis);
    std::copy_n(matrix + 6, 3, result.z_axis);
  } else {
    std::memcpy(
        result.x_axis, latest.x_axis,
        sizeof(result.x_axis));
    std::memcpy(
        result.y_axis, latest.y_axis,
        sizeof(result.y_axis));
    std::memcpy(
        result.z_axis, latest.z_axis,
        sizeof(result.z_axis));
  }
  result.board_state_flags = latest.board_state_flags;
  return result;
}

bool SampleLocalPose(const float map_origin[3], std::int32_t role,
                     const AnimationPose* presentation,
                     PosePacket& packet) {
  trick_pipeline::LiveSpatialSnapshot snapshot;
  const bool have_spatial_snapshot =
      trick_pipeline::CurrentLiveSpatialSnapshot(snapshot);
  if (map_origin == nullptr) {
    return false;
  }
  if (presentation != nullptr &&
      presentation->presentation_root_valid) {
    for (std::size_t component = 0; component < 3; ++component) {
      packet.position[component] =
          presentation->presentation_root_position[component] -
          map_origin[component];
      packet.x_axis[component] =
          presentation->presentation_root_x_axis[component];
      packet.z_axis[component] =
          presentation->presentation_root_z_axis[component];
    }
    packet.sender_time_us = presentation->sender_time_us;
  } else {
    if (!have_spatial_snapshot) {
      return false;
    }
    for (std::size_t component = 0; component < 3; ++component) {
      packet.position[component] =
          std::bit_cast<float>(
              snapshot.position_bits[component]) -
          map_origin[component];
      packet.x_axis[component] =
          std::bit_cast<float>(
              snapshot.x_axis_bits[component]);
      packet.z_axis[component] =
          std::bit_cast<float>(
              snapshot.z_axis_bits[component]);
    }
    packet.sender_time_us = snapshot.sample_time_us;
  }
  if (!Finite3(packet.position) || !Finite3(packet.x_axis) ||
      !Finite3(packet.z_axis)) {
    return false;
  }
  const float lane_spacing =
      static_cast<float>(REXCVAR_GET(skate3_multiplayer_local_lane_spacing));
  const float lane_offset =
      role == 1 ? -lane_spacing * 0.5f : lane_spacing * 0.5f;
  for (std::size_t component = 0; component < 3; ++component) {
    packet.position[component] +=
        packet.x_axis[component] * lane_offset;
  }
  packet.board_state_flags =
      have_spatial_snapshot
          ? snapshot.board_state_flags
          : 0xFFFFFFFFu;
  return true;
}

class Runtime {
 public:
  ~Runtime() { Shutdown(); }

  bool Tick(const char* map_name, const float map_origin[3],
            const AnimationPose* local_animation,
            const AppearanceBlob* local_appearance,
            std::vector<RemotePlayer>& out_remotes) {
    // The explicit local-visuals mode is the isolated multi-process test
    // transport. Steam can still be active in both portable clients under
    // the same account, which would otherwise make them choose the same
    // lobby role and never exercise localhost replication.
    const bool local_test_active =
        REXCVAR_GET(skate3_multiplayer_local_visuals);
    // Session UI owns Steam initialization. The renderer only pumps an
    // already initialized backend; otherwise a failed SteamAPI_InitFlat call
    // can synchronously stall every rendered frame even in solo/local-test
    // play.
    if (!local_test_active && steam::IsInitialized()) {
      steam::Tick();
    }
    std::scoped_lock lock(mutex_);
    const bool steam_active =
        steam::TransportActive() && !local_test_active;
    const bool enabled =
        steam_active || local_test_active;
    const std::int32_t role = steam_active
                                  ? static_cast<std::int32_t>(
                                        steam::LocalRole())
                                  : REXCVAR_GET(
                                        skate3_multiplayer_local_client);
    telemetry_.enabled = enabled && role != 0;
    telemetry_.role = role;
    if (!enabled || role == 0) {
      ShutdownLocked();
      telemetry_.remote_visible = false;
      out_remotes.clear();
      return false;
    }

    const std::int32_t base_port =
        REXCVAR_GET(skate3_multiplayer_local_base_port);
    if (!(steam_active ? EnsureSteam(role)
                       : EnsureSocket(role, base_port))) {
      telemetry_.remote_visible = false;
      out_remotes.clear();
      return false;
    }

    const auto now = Clock::now();
    const std::uint32_t map_hash = HashMapName(map_name);
    std::size_t participant_count = remote_peers_.size() + 1;
#if defined(_WIN32)
    if (using_steam_) {
      participant_count =
          std::max(participant_count, steam_id_by_role_.size());
    } else if (bound_role_ == 1) {
      participant_count =
          std::max(participant_count, host_peers_.size() + 1);
    }
#endif
    network_tuning_ = ResolveNetworkTuning(participant_count);
    relevance_cache_.clear();
    ReceivePackets(now, map_hash);
    PrunePeers(now);
    const std::int32_t send_rate = network_tuning_.pose_rate;
    const auto send_interval = std::chrono::microseconds(
        1000000 / std::max(send_rate, 1));
    if (last_send_ == Clock::time_point{} ||
        now - last_send_ >= send_interval) {
      PosePacket packet;
      if (SampleLocalPose(
              map_origin, role, local_animation, packet) &&
          (packet.sender_time_us == 0 ||
           packet.sender_time_us !=
               last_pose_sample_time_us_)) {
        packet.sender_role = static_cast<std::uint32_t>(role);
        packet.sender_session = session_id_;
        packet.sequence = ++send_sequence_;
        packet.map_hash = map_hash;
        if (packet.sender_time_us == 0) {
          packet.sender_time_us = NowMicroseconds();
        }
        SendPacket(packet, role, base_port);
        last_pose_sample_time_us_ = packet.sender_time_us;
        last_send_ = now;
      }
    }
    const std::int32_t animation_rate =
        network_tuning_.animation_rate;
    const auto animation_interval = std::chrono::microseconds(
        1000000 / std::max(animation_rate, 1));
    if (local_animation != nullptr &&
        !local_animation->tracks.empty() &&
        (local_animation->sender_time_us == 0 ||
         local_animation->sender_time_us !=
             last_animation_sample_time_us_) &&
        (last_animation_send_ == Clock::time_point{} ||
         now - last_animation_send_ >= animation_interval)) {
      SendAnimation(
          *local_animation, map_origin, map_hash, role, base_port);
      last_animation_sample_time_us_ =
          local_animation->sender_time_us;
      last_animation_send_ = now;
    }
    if (local_appearance != nullptr &&
        local_appearance->identity != 0 &&
        local_appearance->bytes != nullptr &&
        !local_appearance->bytes->empty() &&
        (last_appearance_send_ == Clock::time_point{} ||
         now - last_appearance_send_ >=
             std::chrono::milliseconds(4))) {
      SendAppearance(
          *local_appearance, map_hash, role, base_port);
      last_appearance_send_ = now;
    }

    std::vector<std::pair<float, std::uint32_t>> visible_candidates;
    visible_candidates.reserve(remote_peers_.size());
    for (const auto& [remote_role, peer] : remote_peers_) {
      if (peer.samples.empty()) {
        continue;
      }
      float distance_squared = 0.0f;
      if (local_position_valid_) {
        for (std::size_t component = 0; component < 3; ++component) {
          const float delta =
              peer.samples.back().pose.position[component] -
              local_position_[component];
          distance_squared += delta * delta;
        }
      }
      visible_candidates.push_back({distance_squared, remote_role});
    }
    std::sort(
        visible_candidates.begin(), visible_candidates.end(),
        [](const auto& left, const auto& right) {
          return left.first == right.first ? left.second < right.second
                                          : left.first < right.first;
        });
    const std::size_t visual_budget = static_cast<std::size_t>(
        std::max(network_tuning_.relevance_players, 1));
    if (visible_candidates.size() > visual_budget) {
      visible_candidates.resize(visual_budget);
    }
    out_remotes.clear();
    out_remotes.reserve(visible_candidates.size());
    std::uint64_t newest_age_ms = 0;
    bool first_visible = true;
    for (const auto& [distance_squared, remote_role] :
         visible_candidates) {
      (void)distance_squared;
      RemotePeerState& peer = remote_peers_.at(remote_role);
      RemotePlayer remote;
      remote.role = remote_role;
      if (!SmoothRemote(remote_role, peer, now, remote.pose)) {
        continue;
      }
      SmoothRemoteAnimation(
          peer, now, map_origin, remote.animation);
      remote.appearance = peer.appearance;
      if (first_visible) {
        std::memcpy(
            telemetry_.remote_position, remote.pose.position,
            sizeof(telemetry_.remote_position));
        if (!peer.samples.empty()) {
          newest_age_ms = static_cast<std::uint64_t>(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  now - peer.samples.back().received_at)
                  .count());
        }
        first_visible = false;
      }
      out_remotes.push_back(std::move(remote));
    }
    std::sort(
        out_remotes.begin(), out_remotes.end(),
        [](const RemotePlayer& left, const RemotePlayer& right) {
          return left.role < right.role;
        });
    telemetry_.remote_visible = !out_remotes.empty();
    telemetry_.visible_players =
        static_cast<std::uint32_t>(out_remotes.size());
    telemetry_.remote_age_ms = newest_age_ms;
    telemetry_.known_peers =
        static_cast<std::uint32_t>(remote_peers_.size());
    LogRates(now);
    return !out_remotes.empty();
  }

  void Append(std::ostream& out) {
    std::scoped_lock lock(mutex_);
    out << " multiplayer_local_enabled=" << (telemetry_.enabled ? 1 : 0)
        << " multiplayer_local_role=" << telemetry_.role
        << " multiplayer_socket_ready="
        << (telemetry_.socket_ready ? 1 : 0)
        << " multiplayer_remote_visible="
        << (telemetry_.remote_visible ? 1 : 0)
        << " multiplayer_session=" << telemetry_.session
        << " multiplayer_tx_sequence=" << telemetry_.sent_sequence
        << " multiplayer_rx_sequence=" << telemetry_.received_sequence
        << " multiplayer_tx_packets=" << telemetry_.sent_packets
        << " multiplayer_rx_packets=" << telemetry_.received_packets
        << " multiplayer_tx_bytes=" << telemetry_.sent_bytes
        << " multiplayer_rx_bytes=" << telemetry_.received_bytes
        << " multiplayer_tx_root_packets="
        << telemetry_.sent_root_packets
        << " multiplayer_tx_root_bytes="
        << telemetry_.sent_root_bytes
        << " multiplayer_tx_animation_fragments="
        << telemetry_.sent_animation_fragments
        << " multiplayer_tx_animation_bytes="
        << telemetry_.sent_animation_bytes
        << " multiplayer_tx_appearance_chunks="
        << telemetry_.sent_appearance_chunks
        << " multiplayer_tx_appearance_bytes="
        << telemetry_.sent_appearance_bytes
        << " multiplayer_rx_root_packets="
        << telemetry_.received_root_packets
        << " multiplayer_rx_root_bytes="
        << telemetry_.received_root_bytes
        << " multiplayer_rx_animation_fragments="
        << telemetry_.received_animation_fragments
        << " multiplayer_rx_animation_bytes="
        << telemetry_.received_animation_bytes
        << " multiplayer_rx_appearance_chunks="
        << telemetry_.received_appearance_chunks
        << " multiplayer_rx_appearance_bytes="
        << telemetry_.received_appearance_bytes
        << " multiplayer_rejected_packets="
        << telemetry_.rejected_packets
        << " multiplayer_socket_failures=" << telemetry_.socket_failures
        << " multiplayer_remote_age_ms=" << telemetry_.remote_age_ms
        << " multiplayer_tx_animation_frames="
        << telemetry_.sent_animation_frames
        << " multiplayer_rx_animation_frames="
        << telemetry_.received_animation_frames
        << " multiplayer_remote_animation_bones="
        << telemetry_.remote_animation_bones
        << " multiplayer_known_peers=" << telemetry_.known_peers
        << " multiplayer_visible_players="
        << telemetry_.visible_players
        << " multiplayer_relayed_packets="
        << telemetry_.relayed_packets
        << " multiplayer_relevance_drops="
        << telemetry_.relevance_drops
        << " multiplayer_far_presence_packets="
        << telemetry_.far_presence_packets
        << " multiplayer_outbound_peer_resets="
        << telemetry_.outbound_peer_resets
        << " multiplayer_appearance_assembly_timeouts="
        << telemetry_.appearance_assembly_timeouts
        << " multiplayer_appearance_budget_rejections="
        << telemetry_.appearance_budget_rejections
        << " multiplayer_incomplete_appearance_bytes="
        << telemetry_.incomplete_appearance_bytes
        << " multiplayer_animation_present_interpolated="
        << telemetry_.animation_present_interpolated
        << " multiplayer_animation_present_held_latest="
        << telemetry_.animation_present_held_latest
        << " multiplayer_animation_present_held_oldest="
        << telemetry_.animation_present_held_oldest
        << " multiplayer_animation_period_us="
        << telemetry_.animation_period_us
        << " multiplayer_animation_jitter_us="
        << telemetry_.animation_jitter_us
        << " multiplayer_animation_buffered_samples="
        << telemetry_.animation_buffered_samples
        << " multiplayer_remote_x_bits="
        << std::bit_cast<std::uint32_t>(telemetry_.remote_position[0])
        << " multiplayer_remote_y_bits="
        << std::bit_cast<std::uint32_t>(telemetry_.remote_position[1])
        << " multiplayer_remote_z_bits="
        << std::bit_cast<std::uint32_t>(telemetry_.remote_position[2]);
  }

 private:
  void LogRates(Clock::time_point now) {
    constexpr auto kLogInterval = std::chrono::seconds(5);
    if (last_rate_log_ == Clock::time_point{}) {
      last_rate_log_ = now;
      last_rate_snapshot_ = telemetry_;
      return;
    }
    if (now - last_rate_log_ < kLogInterval) {
      return;
    }
    const double seconds =
        std::chrono::duration<double>(now - last_rate_log_).count();
    const auto per_second =
        [seconds](std::uint64_t current, std::uint64_t previous) {
          return static_cast<double>(current - previous) / seconds;
        };
    const double tx_kib = per_second(
        telemetry_.sent_bytes, last_rate_snapshot_.sent_bytes) /
        1024.0;
    const double rx_kib = per_second(
        telemetry_.received_bytes,
        last_rate_snapshot_.received_bytes) /
        1024.0;
    const double tx_pps = per_second(
        telemetry_.sent_packets,
        last_rate_snapshot_.sent_packets);
    const double rx_pps = per_second(
        telemetry_.received_packets,
        last_rate_snapshot_.received_packets);
    const double tx_root_kib = per_second(
        telemetry_.sent_root_bytes,
        last_rate_snapshot_.sent_root_bytes) /
        1024.0;
    const double tx_animation_kib = per_second(
        telemetry_.sent_animation_bytes,
        last_rate_snapshot_.sent_animation_bytes) /
        1024.0;
    const double tx_appearance_kib = per_second(
        telemetry_.sent_appearance_bytes,
        last_rate_snapshot_.sent_appearance_bytes) /
        1024.0;
    const double rx_root_kib = per_second(
        telemetry_.received_root_bytes,
        last_rate_snapshot_.received_root_bytes) /
        1024.0;
    const double rx_animation_kib = per_second(
        telemetry_.received_animation_bytes,
        last_rate_snapshot_.received_animation_bytes) /
        1024.0;
    const double rx_appearance_kib = per_second(
        telemetry_.received_appearance_bytes,
        last_rate_snapshot_.received_appearance_bytes) /
        1024.0;
    const double animation_tx_fps = per_second(
        telemetry_.sent_animation_frames,
        last_rate_snapshot_.sent_animation_frames);
    const double animation_rx_fps = per_second(
        telemetry_.received_animation_frames,
        last_rate_snapshot_.received_animation_frames);
    const double relay_pps = per_second(
        telemetry_.relayed_packets,
        last_rate_snapshot_.relayed_packets);
    const double drop_pps = per_second(
        telemetry_.relevance_drops,
        last_rate_snapshot_.relevance_drops);
    const double animation_interpolated_fps = per_second(
        telemetry_.animation_present_interpolated,
        last_rate_snapshot_.animation_present_interpolated);
    const double animation_held_latest_fps = per_second(
        telemetry_.animation_present_held_latest,
        last_rate_snapshot_.animation_present_held_latest);
    const double animation_held_oldest_fps = per_second(
        telemetry_.animation_present_held_oldest,
        last_rate_snapshot_.animation_present_held_oldest);
    REXLOG_INFO(
        "multiplayer-net: role={} peers={} visible={} quality={} interp={} "
        "rates={}/{}Hz tx={:.1f}KiB/s "
        "rx={:.1f}KiB/s tx={:.1f}pps rx={:.1f}pps anim={:.1f}/{:.1f}fps "
        "classes=tx({:.1f}r/{:.1f}a/{:.1f}p)KiB/s "
        "rx({:.1f}r/{:.1f}a/{:.1f}p)KiB/s "
        "bones={} relay={:.1f}pps relevance_drop={:.1f}pps rejected={} "
        "failures={} peer_resets={} "
        "appearance={:.2f}MiB timeout={} budget_reject={} "
        "present={:.1f}i/{:.1f}new/{:.1f}old fps "
        "timing={:.1f}ms jitter={:.1f}ms buffered={}",
        bound_role_, telemetry_.known_peers,
        telemetry_.visible_players, NetworkQualityName(network_tuning_),
        REXCVAR_GET(skate3_multiplayer_animation_interpolation_mode),
        network_tuning_.pose_rate, network_tuning_.animation_rate,
        tx_kib, rx_kib, tx_pps, rx_pps,
        animation_tx_fps, animation_rx_fps,
        tx_root_kib, tx_animation_kib, tx_appearance_kib,
        rx_root_kib, rx_animation_kib, rx_appearance_kib,
        telemetry_.remote_animation_bones, relay_pps, drop_pps,
        telemetry_.rejected_packets, telemetry_.socket_failures,
        telemetry_.outbound_peer_resets,
        static_cast<double>(
            telemetry_.incomplete_appearance_bytes) /
            (1024.0 * 1024.0),
        telemetry_.appearance_assembly_timeouts,
        telemetry_.appearance_budget_rejections,
        animation_interpolated_fps, animation_held_latest_fps,
        animation_held_oldest_fps,
        static_cast<double>(telemetry_.animation_period_us) / 1000.0,
        static_cast<double>(telemetry_.animation_jitter_us) / 1000.0,
        telemetry_.animation_buffered_samples);
    last_rate_log_ = now;
    last_rate_snapshot_ = telemetry_;
  }

  void ResetOutboundPeerState(std::uint32_t role,
                              std::string_view reason) {
    bool reset = outbound_animation_keyframes_.erase(role) != 0;
    reset |= outbound_appearance_.erase(role) != 0;
    for (auto iterator = far_presence_times_.begin();
         iterator != far_presence_times_.end();) {
      const std::uint32_t source_role =
          static_cast<std::uint32_t>(iterator->first >> 32);
      const std::uint32_t target_role =
          static_cast<std::uint32_t>(iterator->first);
      if (source_role == role || target_role == role) {
        iterator = far_presence_times_.erase(iterator);
        reset = true;
      } else {
        ++iterator;
      }
    }
    for (auto iterator = relevance_cache_.begin();
         iterator != relevance_cache_.end();) {
      const std::uint32_t source_role =
          static_cast<std::uint32_t>(iterator->first >> 32);
      const std::uint32_t target_role =
          static_cast<std::uint32_t>(iterator->first);
      if (source_role == role || target_role == role) {
        iterator = relevance_cache_.erase(iterator);
      } else {
        ++iterator;
      }
    }
    if (!reset) {
      return;
    }
    ++telemetry_.outbound_peer_resets;
    REXLOG_INFO(
        "multiplayer: reset outbound state for role {} ({})",
        role, reason);
  }

  void ForgetPeerGeneration(std::uint32_t role,
                            std::string_view reason) {
    (void)peer_generations_.Forget(role);
    ResetOutboundPeerState(role, reason);
  }

  [[nodiscard]] std::size_t IncompleteAppearanceBytes(
      std::uint32_t excluded_role = 0) const {
    std::size_t total = 0;
    for (const auto& [role, peer] : remote_peers_) {
      if (role == excluded_role ||
          peer.appearance_assembly.identity == 0) {
        continue;
      }
      total += peer.appearance_assembly.bytes.size();
    }
    return total;
  }

  void PrunePeers(Clock::time_point now) {
    constexpr auto kForgetPeerAfter = std::chrono::seconds(5);
    for (auto iterator = remote_peers_.begin();
         iterator != remote_peers_.end();) {
      RemotePeerState& peer = iterator->second;
      if (peer.appearance_assembly.identity != 0 &&
          lifecycle::AppearanceAssemblyExpired<Clock>(
              now, peer.appearance_assembly.last_update)) {
        peer.appearance_assembly = {};
        ++telemetry_.appearance_assembly_timeouts;
      }
      if (peer.appearance_assembly.identity != 0) {
        ++iterator;
        continue;
      }
      Clock::time_point newest = peer.last_packet_at;
      if (!peer.samples.empty()) {
        newest = std::max(
            newest, peer.samples.back().received_at);
      }
      if (!peer.animation_samples.empty()) {
        newest = std::max(
            newest, peer.animation_samples.back().received_at);
      }
      if (newest != Clock::time_point{} &&
          now - newest > kForgetPeerAfter) {
        ForgetPeerGeneration(
            iterator->first, "remote timeout");
        iterator = remote_peers_.erase(iterator);
      } else {
        ++iterator;
      }
    }
#if defined(_WIN32)
    if (bound_role_ == 1) {
      for (auto iterator = host_peers_.begin();
           iterator != host_peers_.end();) {
        if (now - iterator->second.last_seen > kForgetPeerAfter) {
          ForgetPeerGeneration(
              iterator->first, "host peer timeout");
          iterator = host_peers_.erase(iterator);
        } else {
          ++iterator;
        }
      }
    }
#endif
    telemetry_.known_peers =
        static_cast<std::uint32_t>(remote_peers_.size());
    telemetry_.incomplete_appearance_bytes =
        IncompleteAppearanceBytes();
  }

  bool EnsureSteam(std::int32_t role) {
#if defined(_WIN32)
    const steam::State state = steam::GetState();
    if (!state.in_lobby || role <= 0) {
      return false;
    }
    if (!using_steam_ || bound_role_ != role ||
        steam_lobby_id_ != state.lobby_id) {
      ShutdownLocked();
      using_steam_ = true;
      bound_role_ = role;
      steam_lobby_id_ = state.lobby_id;
      session_id_ = static_cast<std::uint32_t>(
          state.local_steam_id ^ (state.local_steam_id >> 32) ^
          state.lobby_id ^ (state.lobby_id >> 32));
      if (session_id_ == 0) {
        session_id_ = 1;
      }
      telemetry_.socket_ready = true;
      telemetry_.session = session_id_;
      REXLOG_INFO(
          "multiplayer: Steam P2P active as role {} in lobby {} "
          "(protocol={} session={})",
          role, state.lobby_id, kProtocolVersion, session_id_);
    }

    std::array<bool, 101> observed_roles{};
    steam_role_by_id_.clear();
    for (const steam::Peer& peer : steam::LobbyPeers()) {
      if (peer.role < 1 || peer.role >= observed_roles.size() ||
          peer.steam_id == 0) {
        continue;
      }
      observed_roles[peer.role] = true;
      steam_id_by_role_[peer.role] = peer.steam_id;
      steam_role_by_id_[peer.steam_id] = peer.role;
      if (peer.role != static_cast<std::uint32_t>(role) &&
          peer_generations_.ObserveTransportIdentity(
              peer.role, peer.steam_id)) {
        ResetOutboundPeerState(
            peer.role, "Steam identity changed");
      }
    }
    for (auto iterator = steam_id_by_role_.begin();
         iterator != steam_id_by_role_.end();) {
      const std::uint32_t previous_role = iterator->first;
      if (previous_role < observed_roles.size() &&
          observed_roles[previous_role]) {
        ++iterator;
        continue;
      }
      if (previous_role != static_cast<std::uint32_t>(role)) {
        ForgetPeerGeneration(
            previous_role, "left Steam lobby");
      }
      iterator = steam_id_by_role_.erase(iterator);
    }
    telemetry_.known_peers = static_cast<std::uint32_t>(
        steam_id_by_role_.empty() ? 0 : steam_id_by_role_.size() - 1);
    return steam_id_by_role_.contains(
        static_cast<std::uint32_t>(role));
#else
    (void)role;
    return false;
#endif
  }

  bool EnsureSocket(std::int32_t role, std::int32_t base_port) {
#if defined(_WIN32)
    if (!using_steam_ && socket_ != INVALID_SOCKET &&
        bound_role_ == role &&
        bound_base_port_ == base_port) {
      return true;
    }
    ShutdownLocked();

    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      ++telemetry_.socket_failures;
      return false;
    }
    winsock_started_ = true;
    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == INVALID_SOCKET) {
      ++telemetry_.socket_failures;
      ShutdownLocked();
      return false;
    }
    // Windows reports an ICMP "port unreachable" from a peer that has not
    // finished binding yet as WSAECONNRESET on a later recvfrom. UDP is
    // connectionless and the next datagram is valid, so disable that legacy
    // Winsock behavior instead of counting hundreds of harmless startup
    // failures.
    BOOL report_udp_resets = FALSE;
    DWORD ioctl_bytes = 0;
    WSAIoctl(
        socket_, SIO_UDP_CONNRESET, &report_udp_resets,
        sizeof(report_udp_resets), nullptr, 0, &ioctl_bytes, nullptr,
        nullptr);
    const int socket_buffer_bytes = 4 * 1024 * 1024;
    setsockopt(
        socket_, SOL_SOCKET, SO_RCVBUF,
        reinterpret_cast<const char*>(&socket_buffer_bytes),
        sizeof(socket_buffer_bytes));
    setsockopt(
        socket_, SOL_SOCKET, SO_SNDBUF,
        reinterpret_cast<const char*>(&socket_buffer_bytes),
        sizeof(socket_buffer_bytes));
    u_long nonblocking = 1;
    if (ioctlsocket(socket_, FIONBIO, &nonblocking) == SOCKET_ERROR) {
      ++telemetry_.socket_failures;
      ShutdownLocked();
      return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port =
        htons(static_cast<u_short>(base_port + role - 1));
    if (bind(socket_, reinterpret_cast<const sockaddr*>(&address),
             sizeof(address)) == SOCKET_ERROR) {
      ++telemetry_.socket_failures;
      REXLOG_ERROR(
          "multiplayer: client {} could not bind localhost UDP port {} "
          "(winsock={})",
          role, base_port + role - 1, WSAGetLastError());
      ShutdownLocked();
      return false;
    }
    bound_role_ = role;
    bound_base_port_ = base_port;
    session_id_ = static_cast<std::uint32_t>(
        NowMicroseconds() ^ (static_cast<std::uint64_t>(
                                 GetCurrentProcessId())
                             << 13));
    telemetry_.socket_ready = true;
    telemetry_.session = session_id_;
    REXLOG_INFO(
        "multiplayer: local client {} listening on 127.0.0.1:{} and sending "
        "to 127.0.0.1:{} (protocol={} session={})",
        role, base_port + role - 1, base_port + (role == 1 ? 1 : 0),
        kProtocolVersion, session_id_);
    return true;
#else
    (void)role;
    (void)base_port;
    ++telemetry_.socket_failures;
    return false;
#endif
  }

  void ReceivePackets(Clock::time_point now, std::uint32_t map_hash) {
#if defined(_WIN32)
    if (using_steam_) {
      for (steam::Message& message : steam::ReceiveMessages(4096)) {
        PacketEndpoint sender;
        sender.steam = true;
        sender.steam_id = message.sender_steam_id;
        ProcessReceivedPacket(
            now, map_hash, message.bytes.data(),
            static_cast<int>(message.bytes.size()), sender);
      }
      return;
    }
    // Animation frames are deliberately split into MTU-safe datagrams. A
    // busy host can therefore receive thousands of packets between rendered
    // frames even though each client only publishes at 20 Hz.
    for (std::size_t attempt = 0; attempt < 4096; ++attempt) {
      std::array<std::byte, 1200> bytes{};
      sockaddr_in sender{};
      int sender_size = sizeof(sender);
      const int received = recvfrom(
          socket_, reinterpret_cast<char*>(bytes.data()),
          static_cast<int>(bytes.size()), 0,
          reinterpret_cast<sockaddr*>(&sender), &sender_size);
      if (received == SOCKET_ERROR) {
        const int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK) {
          ++telemetry_.socket_failures;
        }
        break;
      }
      PacketEndpoint endpoint;
      endpoint.udp = sender;
      ProcessReceivedPacket(
          now, map_hash, bytes.data(), received, endpoint);
    }
#else
    (void)now;
    (void)map_hash;
#endif
  }

#if defined(_WIN32)
  bool SteamSenderValid(std::uint32_t sender_role,
                        const PacketEndpoint& sender) const {
    if (!sender.steam) {
      return true;
    }
    const auto found = steam_role_by_id_.find(sender.steam_id);
    return found != steam_role_by_id_.end() &&
           found->second == sender_role;
  }

  void ProcessReceivedPacket(Clock::time_point now,
                             std::uint32_t map_hash,
                             const std::byte* bytes, int received,
                             const PacketEndpoint& sender) {
    if (bytes == nullptr ||
        received < static_cast<int>(sizeof(std::uint32_t))) {
      ++telemetry_.rejected_packets;
      return;
    }
    telemetry_.received_bytes +=
        static_cast<std::uint64_t>(received);
    std::uint32_t magic = 0;
    std::memcpy(&magic, bytes, sizeof(magic));
    if (magic == kPacketMagic &&
        received == static_cast<int>(sizeof(PosePacket))) {
      PosePacket packet;
      std::memcpy(&packet, bytes, sizeof(packet));
      if (!SteamSenderValid(packet.sender_role, sender)) {
        ++telemetry_.rejected_packets;
        return;
      }
      if (ReceivePosePacket(now, map_hash, packet)) {
        RegisterPeer(
            packet.sender_role, packet.sender_session, sender, now,
            packet.position);
        RelayPacket(
            bytes, received, packet.sender_role,
            /*animation=*/false, now);
      }
    } else if (
        magic == kAnimationPacketMagic &&
        received >= static_cast<int>(
                        offsetof(AnimationFragmentPacket, words))) {
      AnimationFragmentPacket packet;
      std::memcpy(
          &packet, bytes,
          std::min<std::size_t>(
              static_cast<std::size_t>(received), sizeof(packet)));
      if (!SteamSenderValid(packet.sender_role, sender)) {
        ++telemetry_.rejected_packets;
        return;
      }
      if (ReceiveAnimationPacket(now, map_hash, packet, received)) {
        RegisterPeer(
            packet.sender_role, packet.sender_session, sender, now,
            nullptr);
        RelayPacket(
            bytes, received, packet.sender_role,
            /*animation=*/true, now);
      }
    } else if (
        magic == kAppearancePacketMagic &&
        received >= static_cast<int>(
                        offsetof(AppearanceFragmentPacket, bytes))) {
      AppearanceFragmentPacket packet;
      std::memcpy(
          &packet, bytes,
          std::min<std::size_t>(
              static_cast<std::size_t>(received), sizeof(packet)));
      if (!SteamSenderValid(packet.sender_role, sender)) {
        ++telemetry_.rejected_packets;
        return;
      }
      if (ReceiveAppearancePacket(
              now, map_hash, packet, received)) {
        RegisterPeer(
            packet.sender_role, packet.sender_session, sender, now,
            nullptr);
        RelayPacket(
            bytes, received, packet.sender_role,
            /*animation=*/true, now);
      }
    } else {
      ++telemetry_.rejected_packets;
    }
  }
#endif

  bool CommonPacketValid(std::uint16_t version,
                         std::uint32_t sender_role,
                         std::uint32_t sender_session,
                         std::uint32_t packet_map_hash,
                         std::uint32_t map_hash) const {
    return version == kProtocolVersion &&
           sender_session != session_id_ &&
           sender_role != static_cast<std::uint32_t>(bound_role_) &&
           sender_role >= 1 && sender_role <= 100 &&
           packet_map_hash == map_hash;
  }

  void BeginRemoteSession(std::uint32_t role,
                          RemotePeerState& peer,
                          std::uint32_t sender_session) {
    if (peer.session == sender_session) {
      return;
    }
    if (peer_generations_.ObserveProcessSession(
            role, sender_session)) {
      ResetOutboundPeerState(
          role, "process session changed");
    }
    peer = {};
    peer.session = sender_session;
  }

  bool ReceivePosePacket(Clock::time_point now, std::uint32_t map_hash,
                         const PosePacket& packet) {
    if (packet.byte_count != sizeof(packet) ||
        !CommonPacketValid(
            packet.version, packet.sender_role, packet.sender_session,
            packet.map_hash, map_hash)) {
      ++telemetry_.rejected_packets;
      return false;
    }
    RemotePeerState& peer = remote_peers_[packet.sender_role];
    BeginRemoteSession(
        packet.sender_role, peer, packet.sender_session);
    if (!peer.samples.empty() &&
        (!SequenceNewer(
             packet.sequence, peer.samples.back().sequence) ||
         packet.sender_time_us <=
             peer.samples.back().sender_time_us)) {
      ++telemetry_.rejected_packets;
      return false;
    }
    RemotePose pose;
    if (!DecodePacketPose(packet, pose)) {
      ++telemetry_.rejected_packets;
      return false;
    }
    peer.last_packet_at = now;
    const std::uint64_t receive_time_us = NowMicroseconds();
    const std::int64_t observed_clock_offset =
        static_cast<std::int64_t>(receive_time_us) -
        static_cast<std::int64_t>(packet.sender_time_us);
    if (!peer.clock_offset_valid) {
      peer.clock_offset_us = observed_clock_offset;
      peer.minimum_clock_offset_us = observed_clock_offset;
      peer.clock_offset_valid = true;
    } else {
      // One-way queueing only increases the observed offset. Retain the
      // session minimum instead of slowly following delayed packets upward
      // and then snapping down on the next clean arrival. Slew toward a new
      // minimum so even startup path improvements cannot jump the playhead.
      peer.minimum_clock_offset_us =
          std::min(
              peer.minimum_clock_offset_us,
              observed_clock_offset);
      const std::int64_t correction =
          peer.minimum_clock_offset_us -
          peer.clock_offset_us;
      peer.clock_offset_us +=
          std::clamp<std::int64_t>(
              correction, -250, 250);
    }
    peer.samples.push_back(
        {now, packet.sender_time_us, pose, packet.sequence});
    while (peer.samples.size() > kMaximumBufferedSamples) {
      peer.samples.pop_front();
    }
    ++telemetry_.received_packets;
    RecordReceivedPacketClass(kPacketMagic, sizeof(packet));
    telemetry_.received_sequence = packet.sequence;
    if (!peer.announced) {
      peer.announced = true;
      REXLOG_INFO(
          "multiplayer: client {} received remote client {} "
          "(session={} sequence={} map=0x{:08X})",
          bound_role_, packet.sender_role, packet.sender_session,
          packet.sequence, packet.map_hash);
    }
    return true;
  }

  bool ReceiveAnimationPacket(
      Clock::time_point now, std::uint32_t map_hash,
      const AnimationFragmentPacket& packet, int received_bytes) {
    const std::size_t expected_bytes =
        AnimationFragmentByteCount(packet.word_count);
    if (!CommonPacketValid(
            packet.version, packet.sender_role, packet.sender_session,
            packet.map_hash, map_hash) ||
        packet.byte_count != expected_bytes ||
        received_bytes != static_cast<int>(expected_bytes) ||
        packet.fragment_count == 0 ||
        packet.fragment_count > kMaximumAnimationFragments ||
        packet.fragment_index >= packet.fragment_count ||
        packet.total_words == 0 ||
        packet.total_words > kMaximumAnimationFrameWords ||
        packet.word_count == 0 ||
        packet.word_count > kAnimationFragmentWords ||
        packet.word_offset !=
            std::size_t(packet.fragment_index) *
                kAnimationFragmentWords ||
        std::size_t(packet.word_offset) +
                packet.word_count >
            packet.total_words ||
        !Finite3(packet.root_position) ||
        packet.fragment_count !=
            (std::size_t(packet.total_words) +
                 kAnimationFragmentWords - 1) /
                kAnimationFragmentWords) {
      ++telemetry_.rejected_packets;
      return false;
    }
    if (bound_role_ == 1 &&
        !IsHighDetailCached(packet.sender_role, 1)) {
      // The host can relay an already validated compact fragment without
      // allocating and decoding a skeleton it will not render locally.
      ++telemetry_.received_packets;
      RecordReceivedPacketClass(
          kAnimationPacketMagic, expected_bytes);
      return true;
    }
    RemotePeerState& peer = remote_peers_[packet.sender_role];
    BeginRemoteSession(
        packet.sender_role, peer, packet.sender_session);
    ++telemetry_.received_packets;
    RecordReceivedPacketClass(
        kAnimationPacketMagic, expected_bytes);
    peer.last_packet_at = now;
    if (!peer.animation_samples.empty() &&
        !SequenceNewer(
            packet.sequence,
            peer.animation_samples.back().pose.sequence)) {
      return false;
    }
    if (peer.animation_assembly.session == packet.sender_session &&
        peer.animation_assembly.sequence != 0 &&
        packet.sequence != peer.animation_assembly.sequence &&
        !SequenceNewer(
            packet.sequence,
            peer.animation_assembly.sequence)) {
      // Unreliable internet delivery can put a late fragment from an
      // abandoned frame behind fragments of the next frame. Never let that
      // stale fragment replace the newer in-progress assembly.
      return false;
    }
    if (peer.animation_assembly.session != packet.sender_session ||
        peer.animation_assembly.sequence != packet.sequence) {
      peer.animation_assembly = {};
      peer.animation_assembly.received_at = now;
      peer.animation_assembly.session = packet.sender_session;
      peer.animation_assembly.sequence = packet.sequence;
      peer.animation_assembly.sender_time_us = packet.sender_time_us;
      peer.animation_assembly.root_bone = packet.root_bone;
      std::copy_n(
          packet.root_position, 3,
          peer.animation_assembly.root_position);
      peer.animation_assembly.fragment_count =
          packet.fragment_count;
      peer.animation_assembly.total_words =
          packet.total_words;
      peer.animation_assembly.words.resize(
          packet.total_words);
    }
    if (peer.animation_assembly.sender_time_us !=
            packet.sender_time_us ||
        peer.animation_assembly.root_bone != packet.root_bone ||
        peer.animation_assembly.fragment_count !=
            packet.fragment_count ||
        peer.animation_assembly.total_words != packet.total_words ||
        !std::equal(
            peer.animation_assembly.root_position,
            peer.animation_assembly.root_position + 3,
            packet.root_position)) {
      ++telemetry_.rejected_packets;
      peer.animation_assembly = {};
      return false;
    }
    const std::uint32_t fragment_bit =
        std::uint32_t{1} << packet.fragment_index;
    if ((peer.animation_assembly.received_fragments &
         fragment_bit) == 0) {
      std::copy_n(
          packet.words, packet.word_count,
          peer.animation_assembly.words.begin() +
              packet.word_offset);
      peer.animation_assembly.received_fragments |=
          fragment_bit;
    }
    const std::uint32_t complete_mask =
        (std::uint32_t{1} << packet.fragment_count) - 1;
    if (peer.animation_assembly.received_fragments !=
        complete_mask) {
      return true;
    }
    ReceivedAnimationSample complete;
    complete.received_at = now;
    complete.pose.sender_time_us =
        peer.animation_assembly.sender_time_us;
    complete.pose.sequence = peer.animation_assembly.sequence;
    complete.pose.root_bone =
        peer.animation_assembly.root_bone;
    std::memcpy(
        complete.pose.root_position,
        peer.animation_assembly.root_position,
        sizeof(complete.pose.root_position));
    std::uint32_t total_bones = 0;
    if (!DecodeAnimationFrameWords(
            peer.animation_assembly.words,
            peer.animation_assembly.sequence,
            peer.animation_assembly.root_position,
            peer.animation_keyframe,
            complete.pose.tracks, total_bones)) {
      ++telemetry_.rejected_packets;
      peer.animation_assembly = {};
      return false;
    }
    const std::uint64_t arrival_time_us = NowMicroseconds();
    if (peer.last_animation_sender_time_us != 0 &&
        peer.last_animation_arrival_time_us != 0 &&
        complete.pose.sender_time_us >
            peer.last_animation_sender_time_us &&
        SequenceNewer(
            complete.pose.sequence,
            peer.last_animation_sequence)) {
      const std::uint64_t sender_delta =
          complete.pose.sender_time_us -
          peer.last_animation_sender_time_us;
      const std::uint64_t arrival_delta =
          arrival_time_us -
          peer.last_animation_arrival_time_us;
      const std::uint32_t sequence_delta =
          complete.pose.sequence -
          peer.last_animation_sequence;
      const std::int64_t period_sample =
          static_cast<std::int64_t>(
              sender_delta / sequence_delta);
      if (period_sample >= 8000 &&
          period_sample <= 150000) {
        peer.animation_period_us +=
            (period_sample -
             peer.animation_period_us) /
            8;
      }
      const std::int64_t timing_variation =
          arrival_delta >= sender_delta
              ? static_cast<std::int64_t>(
                    arrival_delta - sender_delta)
              : static_cast<std::int64_t>(
                    sender_delta - arrival_delta);
      peer.animation_jitter_us +=
          (timing_variation -
           peer.animation_jitter_us) /
          16;
    }
    peer.last_animation_sender_time_us =
        complete.pose.sender_time_us;
    peer.last_animation_arrival_time_us =
        arrival_time_us;
    peer.last_animation_sequence =
        complete.pose.sequence;
    peer.animation_samples.push_back(std::move(complete));
    while (peer.animation_samples.size() >
           kMaximumBufferedAnimationSamples) {
      peer.animation_samples.pop_front();
    }
    ++telemetry_.received_animation_frames;
    telemetry_.remote_animation_bones = total_bones;
    peer.animation_assembly = {};
    return true;
  }

  bool ReceiveAppearancePacket(
      Clock::time_point now, std::uint32_t map_hash,
      const AppearanceFragmentPacket& packet,
      int received_bytes) {
    const std::size_t expected_bytes =
        AppearanceFragmentByteCount(packet.chunk_bytes);
    const std::size_t expected_chunks =
        (std::size_t(packet.total_bytes) +
             kAppearanceChunkBytes - 1) /
        kAppearanceChunkBytes;
    const std::size_t offset =
        std::size_t(packet.chunk_index) *
        kAppearanceChunkBytes;
    const std::size_t expected_chunk_bytes =
        offset < packet.total_bytes
            ? std::min<std::size_t>(
                  kAppearanceChunkBytes,
                  std::size_t(packet.total_bytes) - offset)
            : 0;
    if (!CommonPacketValid(
            packet.version, packet.sender_role,
            packet.sender_session, packet.map_hash, map_hash) ||
        packet.byte_count != expected_bytes ||
        received_bytes != static_cast<int>(expected_bytes) ||
        packet.appearance_id == 0 ||
        packet.total_bytes == 0 ||
        packet.total_bytes > kMaximumAppearanceBytes ||
        packet.chunk_count == 0 ||
        packet.chunk_count != expected_chunks ||
        packet.chunk_index >= packet.chunk_count ||
        packet.chunk_bytes == 0 ||
        packet.chunk_bytes > kAppearanceChunkBytes ||
        offset >= packet.total_bytes ||
        packet.chunk_bytes != expected_chunk_bytes) {
      ++telemetry_.rejected_packets;
      return false;
    }
    RemotePeerState& peer =
        remote_peers_[packet.sender_role];
    BeginRemoteSession(
        packet.sender_role, peer, packet.sender_session);
    ++telemetry_.received_packets;
    RecordReceivedPacketClass(
        kAppearancePacketMagic, expected_bytes);
    peer.last_packet_at = now;
    AppearanceAssembly& assembly =
        peer.appearance_assembly;
    if (assembly.identity != packet.appearance_id ||
        assembly.total_bytes != packet.total_bytes ||
        assembly.chunk_count != packet.chunk_count) {
      const std::size_t other_incomplete_bytes =
          IncompleteAppearanceBytes(packet.sender_role);
      if (!lifecycle::CanBeginAppearanceAssembly(
              other_incomplete_bytes, packet.total_bytes)) {
        ++telemetry_.appearance_budget_rejections;
        ++telemetry_.rejected_packets;
        return false;
      }
      assembly = {};
      assembly.identity = packet.appearance_id;
      assembly.total_bytes = packet.total_bytes;
      assembly.chunk_count = packet.chunk_count;
      assembly.last_update = now;
      assembly.bytes.resize(packet.total_bytes);
      assembly.received.resize(packet.chunk_count);
    }
    assembly.last_update = now;
    if (!assembly.received[packet.chunk_index]) {
      std::copy_n(
          packet.bytes, packet.chunk_bytes,
          assembly.bytes.begin() + offset);
      assembly.received[packet.chunk_index] = true;
      ++assembly.received_chunks;
    }
    if (assembly.received_chunks == assembly.chunk_count) {
      peer.appearance.identity = assembly.identity;
      peer.appearance.bytes =
          std::make_shared<const std::vector<std::uint8_t>>(
              std::move(assembly.bytes));
      REXLOG_INFO(
          "multiplayer: received appearance role={} id={:016X} "
          "bytes={} chunks={}",
          packet.sender_role, packet.appearance_id,
          packet.total_bytes, packet.chunk_count);
      assembly = {};
    }
    telemetry_.incomplete_appearance_bytes =
        IncompleteAppearanceBytes();
    return true;
  }

  void RegisterPeer(std::uint32_t role, std::uint32_t session,
#if defined(_WIN32)
                    const PacketEndpoint& endpoint,
#else
                    const int& endpoint,
#endif
                    Clock::time_point now, const float* position) {
#if defined(_WIN32)
    if (bound_role_ != 1 || role <= 1 || role > 100) {
      return;
    }
    HostPeer& peer = host_peers_[role];
    if (peer.session != session) {
      if (peer_generations_.ObserveProcessSession(
              role, session)) {
        ResetOutboundPeerState(
            role, "host peer session changed");
      }
      peer = {};
      peer.session = session;
    }
    peer.endpoint = endpoint;
    peer.last_seen = now;
    if (position != nullptr && Finite3(position)) {
      std::copy_n(position, 3, peer.position);
      peer.position_valid = true;
    }
    telemetry_.known_peers =
        static_cast<std::uint32_t>(host_peers_.size());
#else
    (void)role;
    (void)session;
    (void)endpoint;
    (void)now;
    (void)position;
#endif
  }

  bool PositionForRole(std::uint32_t role, const float*& out) const {
#if defined(_WIN32)
    if (role == static_cast<std::uint32_t>(bound_role_) &&
        local_position_valid_) {
      out = local_position_;
      return true;
    }
    const auto remote = remote_peers_.find(role);
    if (remote != remote_peers_.end() &&
        !remote->second.samples.empty()) {
      out = remote->second.samples.back().pose.position;
      return true;
    }
    const auto found = host_peers_.find(role);
    if (found != host_peers_.end() && found->second.position_valid) {
      out = found->second.position;
      return true;
    }
#else
    (void)role;
#endif
    out = nullptr;
    return false;
  }

  bool IsHighDetail(std::uint32_t source_role,
                    std::uint32_t target_role) const {
#if defined(_WIN32)
    const float* source_position = nullptr;
    const float* target_position = nullptr;
    if (!PositionForRole(source_role, source_position) ||
        !PositionForRole(target_role, target_position)) {
      // New peers receive enough data to become visible before their first
      // root packet establishes relevance.
      return true;
    }
    const auto distance_squared = [](const float* left,
                                     const float* right) {
      const float dx = left[0] - right[0];
      const float dy = left[1] - right[1];
      const float dz = left[2] - right[2];
      return dx * dx + dy * dy + dz * dz;
    };
    const float source_distance_squared =
        distance_squared(source_position, target_position);
    const float radius = network_tuning_.relevance_radius;
    if (source_distance_squared > radius * radius) {
      return false;
    }
    const std::size_t budget = static_cast<std::size_t>(
        std::max(network_tuning_.relevance_players, 1));
    std::size_t closer = 0;
    if (source_role != static_cast<std::uint32_t>(bound_role_) &&
        target_role != static_cast<std::uint32_t>(bound_role_) &&
        local_position_valid_ &&
        distance_squared(local_position_, target_position) <
            source_distance_squared) {
      ++closer;
    }
    for (const auto& [candidate_role, candidate] : remote_peers_) {
      if (candidate_role == source_role ||
          candidate_role == target_role ||
          candidate.samples.empty()) {
        continue;
      }
      if (distance_squared(
              candidate.samples.back().pose.position,
              target_position) <
          source_distance_squared) {
        ++closer;
      }
    }
    return closer < budget;
#else
    (void)source_role;
    (void)target_role;
    return true;
#endif
  }

  bool IsHighDetailCached(std::uint32_t source_role,
                          std::uint32_t target_role) {
    const std::uint64_t key =
        (static_cast<std::uint64_t>(source_role) << 32) |
        target_role;
    const auto found = relevance_cache_.find(key);
    if (found != relevance_cache_.end()) {
      return found->second;
    }
    const bool result = IsHighDetail(source_role, target_role);
    relevance_cache_.emplace(key, result);
    return result;
  }

  bool AllowFarPresence(std::uint32_t source_role,
                        std::uint32_t target_role,
                        Clock::time_point now) {
    const std::uint64_t key =
        (static_cast<std::uint64_t>(source_role) << 32) |
        target_role;
    const std::int32_t rate =
        network_tuning_.far_presence_rate;
    const auto interval = std::chrono::microseconds(
        1000000 / std::max(rate, 1));
    Clock::time_point& last = far_presence_times_[key];
    if (last != Clock::time_point{} && now - last < interval) {
      return false;
    }
    last = now;
    ++telemetry_.far_presence_packets;
    return true;
  }

  void RecordSentPacketClass(const void* bytes, int byte_count) {
    if (bytes == nullptr ||
        byte_count < static_cast<int>(sizeof(std::uint32_t))) {
      return;
    }
    std::uint32_t magic = 0;
    std::memcpy(&magic, bytes, sizeof(magic));
    const auto packet_bytes = static_cast<std::uint64_t>(byte_count);
    if (magic == kPacketMagic) {
      ++telemetry_.sent_root_packets;
      telemetry_.sent_root_bytes += packet_bytes;
    } else if (magic == kAnimationPacketMagic) {
      ++telemetry_.sent_animation_fragments;
      telemetry_.sent_animation_bytes += packet_bytes;
    } else if (magic == kAppearancePacketMagic) {
      ++telemetry_.sent_appearance_chunks;
      telemetry_.sent_appearance_bytes += packet_bytes;
    }
  }

  void RecordReceivedPacketClass(std::uint32_t magic,
                                 std::size_t byte_count) {
    const auto packet_bytes = static_cast<std::uint64_t>(byte_count);
    if (magic == kPacketMagic) {
      ++telemetry_.received_root_packets;
      telemetry_.received_root_bytes += packet_bytes;
    } else if (magic == kAnimationPacketMagic) {
      ++telemetry_.received_animation_fragments;
      telemetry_.received_animation_bytes += packet_bytes;
    } else if (magic == kAppearancePacketMagic) {
      ++telemetry_.received_appearance_chunks;
      telemetry_.received_appearance_bytes += packet_bytes;
    }
  }

#if defined(_WIN32)
  bool SendBytes(const void* bytes, int byte_count,
                 const PacketEndpoint& target, bool animation,
                 bool relayed) {
    bool success = false;
    if (target.steam) {
      success = steam::SendPacketToPeer(
          target.steam_id, bytes,
          static_cast<std::size_t>(byte_count), animation);
    } else {
      const int sent = sendto(
          socket_, reinterpret_cast<const char*>(bytes), byte_count, 0,
          reinterpret_cast<const sockaddr*>(&target.udp),
          sizeof(target.udp));
      success = sent == byte_count;
    }
    if (!success) {
      ++telemetry_.socket_failures;
      return false;
    }
    ++telemetry_.sent_packets;
    telemetry_.sent_bytes += static_cast<std::uint64_t>(byte_count);
    RecordSentPacketClass(bytes, byte_count);
    if (relayed) {
      ++telemetry_.relayed_packets;
    }
    return true;
  }

  PacketEndpoint LoopbackTarget(std::int32_t port) const {
    PacketEndpoint target;
    target.udp.sin_family = AF_INET;
    target.udp.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    target.udp.sin_port = htons(static_cast<u_short>(port));
    return target;
  }

  std::vector<std::pair<std::uint32_t, PacketEndpoint>>
  LocalPacketTargets(std::uint32_t source_role, bool animation,
                     Clock::time_point now) {
    std::vector<std::pair<std::uint32_t, PacketEndpoint>> targets;
    if (using_steam_) {
      for (const auto& [target_role, steam_id] : steam_id_by_role_) {
        if (target_role ==
                static_cast<std::uint32_t>(bound_role_) ||
            target_role == source_role) {
          continue;
        }
        const bool detailed =
            IsHighDetailCached(source_role, target_role);
        if (animation && !detailed) {
          ++telemetry_.relevance_drops;
          continue;
        }
        if (!animation && !detailed &&
            !AllowFarPresence(source_role, target_role, now)) {
          ++telemetry_.relevance_drops;
          continue;
        }
        PacketEndpoint target;
        target.steam = true;
        target.steam_id = steam_id;
        targets.push_back({target_role, target});
      }
      return targets;
    }
    if (bound_role_ != 1) {
      targets.push_back(
          {1, LoopbackTarget(bound_base_port_)});
      return targets;
    }
    if (host_peers_.empty()) {
      // Preserve the simple two-client startup: host packets reach slot 2
      // before that process has announced its endpoint.
      targets.push_back(
          {2, LoopbackTarget(bound_base_port_ + 1)});
      return targets;
    }
    for (const auto& [target_role, peer] : host_peers_) {
      const bool detailed =
          IsHighDetailCached(source_role, target_role);
      if (animation && !detailed) {
        ++telemetry_.relevance_drops;
        continue;
      }
      if (!animation && !detailed &&
          !AllowFarPresence(source_role, target_role, now)) {
        ++telemetry_.relevance_drops;
        continue;
      }
      targets.push_back({target_role, peer.endpoint});
    }
    return targets;
  }
#endif

  void RelayPacket(const void* bytes, int byte_count,
                   std::uint32_t source_role, bool animation,
                   Clock::time_point now) {
#if defined(_WIN32)
    if (bound_role_ != 1) {
      return;
    }
    // Steam peers exchange authenticated packets directly. Relaying the
    // same stream through the lobby owner multiplied its upload by every
    // source/receiver pair and duplicated packets after direct fan-out.
    if (using_steam_) {
      return;
    }
    const auto targets =
        LocalPacketTargets(source_role, animation, now);
    for (const auto& [target_role, endpoint] : targets) {
      if (target_role == source_role) {
        continue;
      }
      SendBytes(bytes, byte_count, endpoint, animation, true);
    }
#else
    (void)bytes;
    (void)byte_count;
    (void)source_role;
    (void)animation;
    (void)now;
#endif
  }

  void SendPacket(const PosePacket& packet, std::int32_t role,
                  std::int32_t base_port) {
#if defined(_WIN32)
    (void)base_port;
    std::copy_n(packet.position, 3, local_position_);
    local_position_valid_ = true;
    const auto now = Clock::now();
    const auto targets = LocalPacketTargets(
        static_cast<std::uint32_t>(role), false, now);
    bool sent_any = false;
    for (const auto& [target_role, target] : targets) {
      (void)target_role;
      sent_any |= SendBytes(
          &packet, static_cast<int>(sizeof(packet)), target,
          /*animation=*/false, /*relayed=*/false);
    }
    if (sent_any) {
      telemetry_.sent_sequence = packet.sequence;
    }
#else
    (void)packet;
    (void)role;
    (void)base_port;
#endif
  }

  void SendAnimation(const AnimationPose& pose, const float map_origin[3],
                     std::uint32_t map_hash, std::int32_t role,
                     std::int32_t base_port) {
#if defined(_WIN32)
    if (map_origin == nullptr || pose.tracks.empty() ||
        !Finite3(pose.root_position)) {
      return;
    }
    std::vector<const AnimationTrack*> tracks;
    tracks.reserve(
        std::min<std::size_t>(
            pose.tracks.size(), kMaximumAnimationTracks));
    for (const AnimationTrack& track : pose.tracks) {
      if (track.mesh_key != 0 && track.bone_rows.size() >= 12 &&
          track.bone_rows.size() % 12 == 0) {
        tracks.push_back(&track);
        if (tracks.size() == kMaximumAnimationTracks) {
          break;
        }
      }
    }
    if (tracks.empty()) {
      return;
    }
    const std::uint32_t sequence = ++animation_send_sequence_;
    const std::uint64_t sender_time_us =
        pose.sender_time_us != 0
            ? pose.sender_time_us
            : NowMicroseconds();
    (void)base_port;
    const auto targets = LocalPacketTargets(
        static_cast<std::uint32_t>(role), true, Clock::now());
    if (targets.empty()) {
      return;
    }
    bool complete = true;
    for (const auto& [target_role, target] : targets) {
      std::vector<const AnimationTrack*> target_tracks = tracks;
      const float* target_position = nullptr;
      if (local_position_valid_ &&
          PositionForRole(target_role, target_position)) {
        const float dx =
            local_position_[0] - target_position[0];
        const float dy =
            local_position_[1] - target_position[1];
        const float dz =
            local_position_[2] - target_position[2];
        const float attachment_radius =
            network_tuning_.attachment_radius;
        if (dx * dx + dy * dy + dz * dz >
            attachment_radius * attachment_radius) {
          target_tracks.erase(
              std::remove_if(
                  target_tracks.begin(), target_tracks.end(),
                  [](const AnimationTrack* track) {
                    return track == nullptr ||
                           track->mesh_key !=
                               kCanonicalSkeletonTrackKey;
                  }),
              target_tracks.end());
        }
      }
      QuantizedAnimationFrame proposed_keyframe =
          outbound_animation_keyframes_[target_role];
      std::vector<std::uint16_t> frame_words;
      if (!BuildAnimationFrameWords(
              pose, target_tracks, sequence, proposed_keyframe,
              frame_words)) {
        complete = false;
        continue;
      }
      const std::size_t fragment_count =
          (frame_words.size() +
               kAnimationFragmentWords - 1) /
          kAnimationFragmentWords;
      bool target_complete = true;
      for (std::size_t fragment_index = 0;
           fragment_index < fragment_count; ++fragment_index) {
        AnimationFragmentPacket packet;
        packet.sender_role = static_cast<std::uint32_t>(role);
        packet.sender_session = session_id_;
        packet.sequence = sequence;
        packet.map_hash = map_hash;
        packet.sender_time_us = sender_time_us;
        for (std::size_t component = 0; component < 3;
             ++component) {
          packet.root_position[component] =
              pose.root_position[component] -
              map_origin[component];
        }
        packet.root_bone = pose.root_bone;
        packet.fragment_index =
            static_cast<std::uint16_t>(fragment_index);
        packet.fragment_count =
            static_cast<std::uint16_t>(fragment_count);
        const std::size_t offset =
            fragment_index * kAnimationFragmentWords;
        packet.word_offset =
            static_cast<std::uint16_t>(offset);
        packet.total_words =
            static_cast<std::uint16_t>(frame_words.size());
        packet.word_count = static_cast<std::uint16_t>(
            std::min<std::size_t>(
                kAnimationFragmentWords,
                frame_words.size() - offset));
        std::copy_n(
            frame_words.begin() + offset,
            packet.word_count, packet.words);
        packet.byte_count = static_cast<std::uint16_t>(
            AnimationFragmentByteCount(packet.word_count));
        target_complete &=
            SendBytes(&packet, packet.byte_count, target,
                      /*animation=*/true, /*relayed=*/false);
      }
      if (target_complete) {
        outbound_animation_keyframes_[target_role] =
            std::move(proposed_keyframe);
      }
      complete &= target_complete;
    }
    if (complete) {
      ++telemetry_.sent_animation_frames;
    }
#else
    (void)pose;
    (void)map_origin;
    (void)map_hash;
    (void)role;
    (void)base_port;
#endif
  }

  void SendAppearance(
      const AppearanceBlob& appearance,
      std::uint32_t map_hash, std::int32_t role,
      std::int32_t base_port) {
#if defined(_WIN32)
    (void)base_port;
    if (appearance.identity == 0 ||
        appearance.bytes == nullptr ||
        appearance.bytes->empty() ||
        appearance.bytes->size() >
            kMaximumAppearanceBytes) {
      return;
    }
    const auto targets = LocalPacketTargets(
        static_cast<std::uint32_t>(role),
        /*animation=*/true, Clock::now());
    const std::size_t chunk_count =
        (appearance.bytes->size() +
             kAppearanceChunkBytes - 1) /
        kAppearanceChunkBytes;
    const Clock::time_point now = Clock::now();
    for (const auto& [target_role, target] : targets) {
      OutboundAppearanceState& state =
          outbound_appearance_[target_role];
      if (state.identity != appearance.identity) {
        state = {};
        state.identity = appearance.identity;
      }
      if (state.next_chunk >= chunk_count) {
        // Steam's reliable channel needs one pass. Localhost UDP is allowed
        // to drop datagrams, and a multi-thousand-chunk appearance cannot
        // require perfect delivery. Repeat the complete stream twice; the
        // receiver keeps its chunk bitmap, so either retry fills only the
        // holes from the previous pass.
        if (using_steam_ ||
            state.completed_passes >= 3 ||
            now < state.retry_after) {
          continue;
        }
        state.next_chunk = 0;
      }
      // Burst localhost transfers so the fallback is visible for seconds,
      // not tens of seconds. Steam stays at one chunk per tick per peer to
      // keep a ten-player join from multiplying every sender's upload.
      const std::size_t burst_chunks =
          using_steam_ ? 1u : 4u;
      for (std::size_t burst = 0;
           burst < burst_chunks &&
           state.next_chunk < chunk_count;
           ++burst) {
        const std::size_t offset =
            std::size_t(state.next_chunk) *
            kAppearanceChunkBytes;
        AppearanceFragmentPacket packet;
        packet.sender_role =
            static_cast<std::uint32_t>(role);
        packet.sender_session = session_id_;
        packet.map_hash = map_hash;
        packet.appearance_id = appearance.identity;
        packet.total_bytes = static_cast<std::uint32_t>(
            appearance.bytes->size());
        packet.chunk_index = state.next_chunk;
        packet.chunk_count =
            static_cast<std::uint16_t>(chunk_count);
        packet.chunk_bytes =
            static_cast<std::uint16_t>(
                std::min<std::size_t>(
                    kAppearanceChunkBytes,
                    appearance.bytes->size() - offset));
        std::copy_n(
            appearance.bytes->begin() + offset,
            packet.chunk_bytes, packet.bytes);
        packet.byte_count = static_cast<std::uint16_t>(
            AppearanceFragmentByteCount(packet.chunk_bytes));
        if (!SendBytes(
                &packet, packet.byte_count, target,
                /*animation=*/true, /*relayed=*/false)) {
          break;
        }
        ++state.next_chunk;
      }
      if (state.next_chunk >= chunk_count) {
        ++state.completed_passes;
        state.retry_after =
            now + std::chrono::milliseconds(250);
      }
    }
#else
    (void)appearance;
    (void)map_hash;
    (void)role;
    (void)base_port;
#endif
  }

  bool SmoothRemote(std::uint32_t remote_role, RemotePeerState& peer,
                    Clock::time_point now, RemotePose& out) {
    const std::int64_t interpolation_delay_us =
        PresentationDelayMicroseconds(
            peer, network_tuning_.interpolation_ms);
    const auto interpolation_delay =
        std::chrono::microseconds(interpolation_delay_us);
    while (!peer.samples.empty() &&
           now - peer.samples.front().received_at >
               kRemoteTimeout + interpolation_delay) {
      peer.samples.pop_front();
    }
    if (peer.samples.empty() ||
        now - peer.samples.back().received_at > kRemoteTimeout) {
      peer.announced = false;
      return false;
    }
    const std::int64_t local_now_us =
        static_cast<std::int64_t>(NowMicroseconds());
    if (peer.clock_offset_valid &&
        peer.animation_samples.size() >= 2 &&
        peer.minimum_clock_offset_us !=
            std::numeric_limits<std::int64_t>::max() &&
        peer.minimum_clock_offset_us < peer.clock_offset_us) {
      constexpr std::int64_t kClockRebaseThresholdUs = 2000;
      const std::int64_t oldest_animation_time_us =
          static_cast<std::int64_t>(
              peer.animation_samples.front().pose.sender_time_us);
      const std::int64_t newest_animation_time_us =
          static_cast<std::int64_t>(
              peer.animation_samples.back().pose.sender_time_us);
      const std::int64_t current_target_time_us =
          local_now_us - peer.clock_offset_us -
          interpolation_delay_us;
      const std::int64_t rebased_target_time_us =
          local_now_us - peer.minimum_clock_offset_us -
          interpolation_delay_us;
      // A delayed first pose can seed the clock offset far above the clean
      // session minimum. The normal 0.25 ms-per-packet slew then leaves both
      // root and skeleton playback pinned to the oldest buffered frame for
      // minutes. Rebase only when the current cursor is demonstrably behind
      // the retained animation timeline and the best observed offset would
      // place it inside that same timeline.
      if (current_target_time_us + kClockRebaseThresholdUs <
              oldest_animation_time_us &&
          rebased_target_time_us + kClockRebaseThresholdUs >=
              oldest_animation_time_us &&
          rebased_target_time_us <=
              newest_animation_time_us +
                  kClockRebaseThresholdUs) {
        const std::int64_t previous_offset_us =
            peer.clock_offset_us;
        peer.clock_offset_us =
            peer.minimum_clock_offset_us;
        if (peer.clock_rebase_count < 4) {
          REXLOG_INFO(
              "multiplayer-clock: receiver={} remote={} rebased "
              "offset={:.1f}->{:.1f}ms stale={:.1f}ms",
              bound_role_, remote_role,
              static_cast<double>(previous_offset_us) / 1000.0,
              static_cast<double>(peer.clock_offset_us) / 1000.0,
              static_cast<double>(
                  oldest_animation_time_us -
                  current_target_time_us) /
                  1000.0);
        }
        ++peer.clock_rebase_count;
      }
    }
    const std::int64_t target_sender_time_us =
        local_now_us - peer.clock_offset_us -
        interpolation_delay_us;
    if (target_sender_time_us <=
        static_cast<std::int64_t>(
            peer.samples.front().sender_time_us)) {
      out = peer.samples.front().pose;
      return true;
    }
    for (std::size_t index = 1; index < peer.samples.size(); ++index) {
      if (target_sender_time_us <=
          static_cast<std::int64_t>(
              peer.samples[index].sender_time_us)) {
        const std::uint64_t span =
            peer.samples[index].sender_time_us -
            peer.samples[index - 1].sender_time_us;
        const std::int64_t elapsed =
            target_sender_time_us -
            static_cast<std::int64_t>(
                peer.samples[index - 1].sender_time_us);
        const float amount =
            span == 0
                ? 1.0f
                : static_cast<float>(elapsed) /
                      static_cast<float>(span);
        out = InterpolatePose(peer.samples[index - 1].pose,
                              peer.samples[index].pose, amount);
        return true;
      }
    }
    if (peer.samples.size() >= 2) {
      const ReceivedSample& previous =
          peer.samples[peer.samples.size() - 2];
      const ReceivedSample& latest =
          peer.samples.back();
      const std::uint64_t span =
          latest.sender_time_us - previous.sender_time_us;
      const std::int64_t ahead =
          target_sender_time_us -
          static_cast<std::int64_t>(
              latest.sender_time_us);
      // Keep short network stalls moving without allowing prediction to
      // run away. At the normal 60 Hz root rate this covers at most two
      // missing samples; low-rate far-presence updates are capped at 100 ms.
      const std::int64_t maximum_ahead =
          std::min<std::int64_t>(
              100000,
              static_cast<std::int64_t>(span) * 2);
      if (span != 0 && ahead > 0 &&
          ahead <= maximum_ahead) {
        out = ExtrapolatePose(
            previous.pose, latest.pose,
            static_cast<float>(ahead) /
                static_cast<float>(span));
        return true;
      }
    }
    out = peer.samples.back().pose;
    return true;
  }

  bool SmoothRemoteAnimation(RemotePeerState& peer,
                             Clock::time_point now,
                             const float map_origin[3],
                             AnimationPose& out) {
    const auto interpolation_delay =
        std::chrono::microseconds(
            PresentationDelayMicroseconds(
                peer, network_tuning_.interpolation_ms));
    while (!peer.animation_samples.empty() &&
           now - peer.animation_samples.front().received_at >
               kRemoteTimeout + interpolation_delay) {
      peer.animation_samples.pop_front();
    }
    if (map_origin == nullptr || peer.animation_samples.empty() ||
        now - peer.animation_samples.back().received_at >
            kRemoteTimeout ||
        !peer.clock_offset_valid) {
      return false;
    }
    const std::int64_t target_sender_time_us =
        static_cast<std::int64_t>(NowMicroseconds()) -
        peer.clock_offset_us -
        std::chrono::duration_cast<std::chrono::microseconds>(
            interpolation_delay)
            .count();
    const AnimationPose* first =
        &peer.animation_samples.front().pose;
    const AnimationPose* second = first;
    const AnimationPose* previous = first;
    const AnimationPose* next = second;
    float amount = 0.0f;
    telemetry_.animation_period_us = peer.animation_period_us;
    telemetry_.animation_jitter_us = peer.animation_jitter_us;
    telemetry_.animation_buffered_samples =
        static_cast<std::uint32_t>(peer.animation_samples.size());
    if (target_sender_time_us <=
        static_cast<std::int64_t>(
            peer.animation_samples.front().pose.sender_time_us)) {
      ++telemetry_.animation_present_held_oldest;
    } else if (target_sender_time_us >=
        static_cast<std::int64_t>(
            peer.animation_samples.back().pose.sender_time_us)) {
      first = second = &peer.animation_samples.back().pose;
      previous = next = second;
      ++telemetry_.animation_present_held_latest;
    } else {
      for (std::size_t index = 1;
           index < peer.animation_samples.size(); ++index) {
        const AnimationPose& candidate =
            peer.animation_samples[index].pose;
        if (target_sender_time_us <=
            static_cast<std::int64_t>(candidate.sender_time_us)) {
          first = &peer.animation_samples[index - 1].pose;
          second = &candidate;
          previous =
              index >= 2
                  ? &peer.animation_samples[index - 2].pose
                  : first;
          next =
              index + 1 < peer.animation_samples.size()
                  ? &peer.animation_samples[index + 1].pose
                  : second;
          const std::uint64_t span =
              second->sender_time_us - first->sender_time_us;
          const std::int64_t elapsed =
              target_sender_time_us -
              static_cast<std::int64_t>(first->sender_time_us);
          amount = span == 0
                       ? 1.0f
                       : std::clamp(
                             static_cast<float>(elapsed) /
                                 static_cast<float>(span),
                             0.0f, 1.0f);
          ++telemetry_.animation_present_interpolated;
          break;
        }
      }
    }
    if (second->tracks.empty()) {
      return false;
    }
    out.sender_time_us = second->sender_time_us;
    out.sequence = second->sequence;
    out.root_bone = second->root_bone;
    for (std::size_t component = 0; component < 3; ++component) {
      out.root_position[component] =
          first->root_position[component] +
          (second->root_position[component] -
           first->root_position[component]) *
              amount +
          map_origin[component];
    }
    out.tracks.clear();
    out.tracks.reserve(second->tracks.size());
    for (const AnimationTrack& second_track : second->tracks) {
      const AnimationTrack* first_track = nullptr;
      const AnimationTrack* previous_track = nullptr;
      const AnimationTrack* next_track = nullptr;
      for (const AnimationTrack& candidate : first->tracks) {
        if (candidate.mesh_key == second_track.mesh_key &&
            candidate.bone_rows.size() ==
                second_track.bone_rows.size()) {
          first_track = &candidate;
          break;
        }
      }
      for (const AnimationTrack& candidate : previous->tracks) {
        if (candidate.mesh_key == second_track.mesh_key &&
            candidate.bone_rows.size() ==
                second_track.bone_rows.size()) {
          previous_track = &candidate;
          break;
        }
      }
      for (const AnimationTrack& candidate : next->tracks) {
        if (candidate.mesh_key == second_track.mesh_key &&
            candidate.bone_rows.size() ==
                second_track.bone_rows.size()) {
          next_track = &candidate;
          break;
        }
      }
      AnimationTrack output;
      output.mesh_key = second_track.mesh_key;
      output.bone_rows.resize(second_track.bone_rows.size());
      if (first_track == nullptr) {
        output.bone_rows = second_track.bone_rows;
      } else {
        if (previous_track == nullptr) {
          previous_track = first_track;
        }
        if (next_track == nullptr) {
          next_track = &second_track;
        }
        const std::size_t bone_count =
            output.bone_rows.size() / 12;
        for (std::size_t bone = 0; bone < bone_count; ++bone) {
          const float* first_bone =
              first_track->bone_rows.data() + bone * 12;
          const float* second_bone =
              second_track.bone_rows.data() + bone * 12;
          float* output_bone =
              output.bone_rows.data() + bone * 12;
          const std::int32_t interpolation_mode = std::clamp(
              REXCVAR_GET(
                  skate3_multiplayer_animation_interpolation_mode),
              0, 2);
          if (interpolation_mode == 0) {
            std::copy_n(second_bone, 12, output_bone);
          } else if (interpolation_mode == 1) {
            InterpolateAffine(
                first_bone, second_bone, amount, output_bone);
          } else {
            // Every transmitted row is a model-to-world skinning affine,
            // not an independently positioned joint. Its translation
            // includes inverse-bind pivot compensation (p - R*p), so this
            // path interpolates the complete rigid transform on SE(3).
            InterpolateAttachmentAffine(
                first_bone, second_bone, amount, output_bone);
          }
        }
      }
      for (std::size_t bone = 0;
           bone < output.bone_rows.size() / 12; ++bone) {
        output.bone_rows[bone * 12 + 3] += map_origin[0];
        output.bone_rows[bone * 12 + 7] += map_origin[1];
        output.bone_rows[bone * 12 + 11] += map_origin[2];
      }
      out.tracks.push_back(std::move(output));
    }
    return true;
  }

  void Shutdown() {
    std::scoped_lock lock(mutex_);
    ShutdownLocked();
  }

  void ShutdownLocked() {
#if defined(_WIN32)
    if (socket_ != INVALID_SOCKET) {
      closesocket(socket_);
      socket_ = INVALID_SOCKET;
    }
    if (winsock_started_) {
      WSACleanup();
      winsock_started_ = false;
    }
    using_steam_ = false;
    steam_lobby_id_ = 0;
    steam_id_by_role_.clear();
    steam_role_by_id_.clear();
#endif
    bound_role_ = 0;
    bound_base_port_ = 0;
    session_id_ = 0;
    send_sequence_ = 0;
    animation_send_sequence_ = 0;
    outbound_animation_keyframes_.clear();
    outbound_appearance_.clear();
    peer_generations_.Clear();
    remote_peers_.clear();
#if defined(_WIN32)
    host_peers_.clear();
#endif
    far_presence_times_.clear();
    relevance_cache_.clear();
    std::fill_n(local_position_, 3, 0.0f);
    local_position_valid_ = false;
    last_send_ = {};
    last_pose_sample_time_us_ = 0;
    last_animation_send_ = {};
    last_animation_sample_time_us_ = 0;
    last_appearance_send_ = {};
    last_rate_log_ = {};
    last_rate_snapshot_ = {};
    telemetry_.socket_ready = false;
    telemetry_.session = 0;
    telemetry_.remote_animation_bones = 0;
    telemetry_.known_peers = 0;
    telemetry_.incomplete_appearance_bytes = 0;
  }

  std::mutex mutex_;
#if defined(_WIN32)
  SOCKET socket_ = INVALID_SOCKET;
  bool winsock_started_ = false;
  bool using_steam_ = false;
  std::uint64_t steam_lobby_id_ = 0;
  std::unordered_map<std::uint32_t, std::uint64_t>
      steam_id_by_role_;
  std::unordered_map<std::uint64_t, std::uint32_t>
      steam_role_by_id_;
#endif
  std::int32_t bound_role_ = 0;
  std::int32_t bound_base_port_ = 0;
  std::uint32_t session_id_ = 0;
  std::uint32_t send_sequence_ = 0;
  std::uint32_t animation_send_sequence_ = 0;
  std::unordered_map<std::uint32_t, QuantizedAnimationFrame>
      outbound_animation_keyframes_;
  std::unordered_map<std::uint32_t, OutboundAppearanceState>
      outbound_appearance_;
  lifecycle::PeerGenerationTracker peer_generations_;
  Clock::time_point last_send_{};
  std::uint64_t last_pose_sample_time_us_ = 0;
  Clock::time_point last_animation_send_{};
  std::uint64_t last_animation_sample_time_us_ = 0;
  Clock::time_point last_appearance_send_{};
  Clock::time_point last_rate_log_{};
  std::unordered_map<std::uint32_t, RemotePeerState> remote_peers_;
#if defined(_WIN32)
  std::unordered_map<std::uint32_t, HostPeer> host_peers_;
#endif
  std::unordered_map<std::uint64_t, Clock::time_point>
      far_presence_times_;
  std::unordered_map<std::uint64_t, bool> relevance_cache_;
  float local_position_[3] = {};
  bool local_position_valid_ = false;
  NetworkTuning network_tuning_;
  TelemetrySnapshot telemetry_;
  TelemetrySnapshot last_rate_snapshot_;
};

Runtime& ActiveRuntime() {
  static Runtime runtime;
  return runtime;
}

}  // namespace

bool TickLocalVisuals(const char* map_name,
                      const float map_render_origin[3],
                      const AnimationPose* local_animation,
                      const AppearanceBlob* local_appearance,
                      std::vector<RemotePlayer>& out_remotes) {
  return ActiveRuntime().Tick(
      map_name, map_render_origin, local_animation,
      local_appearance, out_remotes);
}

void AppendTelemetry(std::ostream& out) {
  ActiveRuntime().Append(out);
}

}  // namespace skate3::multiplayer
