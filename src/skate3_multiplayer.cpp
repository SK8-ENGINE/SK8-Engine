#include "skate3_multiplayer.h"

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
    skate3_multiplayer_local_send_rate, 60, "Skate 3",
    "Pose packets sent per second by the localhost multiplayer transport.")
    .range(10, 120)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    skate3_multiplayer_local_animation_rate, 20, "Skate 3",
    "Completed skeletal-pose frames sent per second by the localhost "
    "multiplayer transport.")
    .range(10, 60)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    skate3_multiplayer_local_interpolation_ms, 50, "Skate 3",
    "Remote-pose buffer duration. Larger values tolerate more jitter but make "
    "the other player appear further behind.")
    .range(0, 250)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    skate3_multiplayer_relevance_radius, 80.0, "Skate 3",
    "Maximum map-space distance for high-detail remote animation routing by "
    "the logical host.")
    .range(5.0, 1000.0)
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

constexpr std::uint32_t kPacketMagic = 0x504D334Bu;  // "K3MP"
constexpr std::uint32_t kAnimationPacketMagic = 0x414D334Bu;  // "K3MA"
constexpr std::uint16_t kProtocolVersion = 2;
constexpr auto kRemoteTimeout = std::chrono::milliseconds(1500);
constexpr std::size_t kMaximumBufferedSamples = 16;
constexpr std::size_t kMaximumBufferedAnimationSamples = 8;
constexpr std::uint16_t kMaximumAnimationBones = 128;
constexpr std::uint16_t kMaximumAnimationTracks = 32;
constexpr std::uint16_t kAnimationFragmentComponents = 480;
constexpr float kAnimationTranslationScale = 1024.0f;

#pragma pack(push, 1)
struct PosePacket {
  std::uint32_t magic = kPacketMagic;
  std::uint16_t version = kProtocolVersion;
  std::uint16_t byte_count = sizeof(PosePacket);
  std::uint32_t sender_role = 0;
  std::uint32_t sender_session = 0;
  std::uint32_t sequence = 0;
  std::uint32_t map_hash = 0;
  std::uint64_t sender_time_us = 0;
  float position[3] = {};
  float x_axis[3] = {};
  float z_axis[3] = {};
  std::uint32_t board_state_flags = 0xFFFFFFFFu;
};

struct AnimationFragmentPacket {
  std::uint32_t magic = kAnimationPacketMagic;
  std::uint16_t version = kProtocolVersion;
  std::uint16_t byte_count = 0;
  std::uint32_t sender_role = 0;
  std::uint32_t sender_session = 0;
  std::uint32_t sequence = 0;
  std::uint32_t map_hash = 0;
  std::uint64_t sender_time_us = 0;
  float root_position[3] = {};
  std::uint32_t mesh_key = 0;
  std::uint16_t bone_count = 0;
  std::uint16_t track_index = 0;
  std::uint16_t track_count = 0;
  std::uint16_t fragment_index = 0;
  std::uint16_t fragment_count = 0;
  std::uint16_t float_count = 0;
  std::uint16_t rows[kAnimationFragmentComponents] = {};
};
#pragma pack(pop)

static_assert(sizeof(PosePacket) == 72);
static_assert(offsetof(AnimationFragmentPacket, rows) == 60);
static_assert(sizeof(AnimationFragmentPacket) == 1020);

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

struct AnimationTrackAssembly {
  std::uint32_t mesh_key = 0;
  std::uint16_t bone_count = 0;
  std::uint16_t fragment_count = 0;
  std::uint64_t received_fragments = 0;
  std::vector<float> rows;
};

struct AnimationAssembly {
  Clock::time_point received_at{};
  std::uint32_t session = 0;
  std::uint32_t sequence = 0;
  std::uint64_t sender_time_us = 0;
  std::uint16_t track_count = 0;
  std::uint64_t received_tracks = 0;
  std::vector<AnimationTrackAssembly> tracks;
};

struct RemotePeerState {
  std::uint32_t session = 0;
  std::int64_t clock_offset_us = 0;
  bool clock_offset_valid = false;
  bool announced = false;
  std::deque<ReceivedSample> samples;
  std::deque<ReceivedAnimationSample> animation_samples;
  AnimationAssembly animation_assembly;
};

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
    result.x_axis[component] =
        first.x_axis[component] +
        (second.x_axis[component] - first.x_axis[component]) * amount;
    result.z_axis[component] =
        first.z_axis[component] +
        (second.z_axis[component] - first.z_axis[component]) * amount;
  }
  if (!Normalize(result.x_axis)) {
    std::memcpy(result.x_axis, second.x_axis, sizeof(result.x_axis));
  }
  const float projection = Dot(result.z_axis, result.x_axis);
  for (std::size_t component = 0; component < 3; ++component) {
    result.z_axis[component] -= result.x_axis[component] * projection;
  }
  if (!Normalize(result.z_axis)) {
    std::memcpy(result.z_axis, second.z_axis, sizeof(result.z_axis));
  }
  Cross(result.z_axis, result.x_axis, result.y_axis);
  Normalize(result.y_axis);
  result.board_state_flags = second.board_state_flags;
  return result;
}

bool SampleLocalPose(const float map_origin[3], std::int32_t role,
                     PosePacket& packet) {
  trick_pipeline::LiveSpatialSnapshot snapshot;
  if (map_origin == nullptr ||
      !trick_pipeline::CurrentLiveSpatialSnapshot(snapshot)) {
    return false;
  }
  for (std::size_t component = 0; component < 3; ++component) {
    packet.position[component] =
        std::bit_cast<float>(snapshot.position_bits[component]) -
        map_origin[component];
    packet.x_axis[component] =
        std::bit_cast<float>(snapshot.x_axis_bits[component]);
    packet.z_axis[component] =
        std::bit_cast<float>(snapshot.z_axis_bits[component]);
  }
  if (!Finite3(packet.position) || !Finite3(packet.x_axis) ||
      !Finite3(packet.z_axis)) {
    return false;
  }
  const float lane_spacing =
      static_cast<float>(REXCVAR_GET(skate3_multiplayer_local_lane_spacing));
  packet.position[0] +=
      role == 1 ? -lane_spacing * 0.5f : lane_spacing * 0.5f;
  packet.board_state_flags = snapshot.board_state_flags;
  return true;
}

class Runtime {
 public:
  ~Runtime() { Shutdown(); }

  bool Tick(const char* map_name, const float map_origin[3],
            const AnimationPose* local_animation,
            std::vector<RemotePlayer>& out_remotes) {
    steam::Tick();
    std::scoped_lock lock(mutex_);
    const bool steam_active = steam::TransportActive();
    const bool enabled =
        steam_active ||
        REXCVAR_GET(skate3_multiplayer_local_visuals);
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
    relevance_cache_.clear();
    ReceivePackets(now, map_hash);
    PrunePeers(now);
    const std::int32_t send_rate =
        REXCVAR_GET(skate3_multiplayer_local_send_rate);
    const auto send_interval = std::chrono::microseconds(
        1000000 / std::max(send_rate, 1));
    if (last_send_ == Clock::time_point{} ||
        now - last_send_ >= send_interval) {
      PosePacket packet;
      if (SampleLocalPose(map_origin, role, packet)) {
        packet.sender_role = static_cast<std::uint32_t>(role);
        packet.sender_session = session_id_;
        packet.sequence = ++send_sequence_;
        packet.map_hash = map_hash;
        packet.sender_time_us = NowMicroseconds();
        SendPacket(packet, role, base_port);
        last_send_ = now;
      }
    }
    const std::int32_t animation_rate =
        REXCVAR_GET(skate3_multiplayer_local_animation_rate);
    const auto animation_interval = std::chrono::microseconds(
        1000000 / std::max(animation_rate, 1));
    if (local_animation != nullptr &&
        !local_animation->tracks.empty() &&
        (last_animation_send_ == Clock::time_point{} ||
         now - last_animation_send_ >= animation_interval)) {
      SendAnimation(
          *local_animation, map_origin, map_hash, role, base_port);
      last_animation_send_ = now;
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
        std::max(
            REXCVAR_GET(skate3_multiplayer_relevance_players), 1));
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
      if (!SmoothRemote(peer, now, remote.pose)) {
        continue;
      }
      SmoothRemoteAnimation(
          peer, now, map_origin, remote.animation);
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
    REXLOG_INFO(
        "multiplayer-net: role={} peers={} visible={} tx={:.1f}KiB/s "
        "rx={:.1f}KiB/s tx={:.1f}pps rx={:.1f}pps anim={:.1f}/{:.1f}fps "
        "bones={} relay={:.1f}pps relevance_drop={:.1f}pps rejected={} "
        "failures={}",
        bound_role_, telemetry_.known_peers,
        telemetry_.visible_players, tx_kib, rx_kib, tx_pps, rx_pps,
        animation_tx_fps, animation_rx_fps,
        telemetry_.remote_animation_bones, relay_pps, drop_pps,
        telemetry_.rejected_packets, telemetry_.socket_failures);
    last_rate_log_ = now;
    last_rate_snapshot_ = telemetry_;
  }

  void PrunePeers(Clock::time_point now) {
    constexpr auto kForgetPeerAfter = std::chrono::seconds(5);
    for (auto iterator = remote_peers_.begin();
         iterator != remote_peers_.end();) {
      const RemotePeerState& peer = iterator->second;
      const Clock::time_point newest =
          peer.samples.empty()
              ? (peer.animation_samples.empty()
                     ? Clock::time_point{}
                     : peer.animation_samples.back().received_at)
              : peer.samples.back().received_at;
      if (newest != Clock::time_point{} &&
          now - newest > kForgetPeerAfter) {
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
          iterator = host_peers_.erase(iterator);
        } else {
          ++iterator;
        }
      }
    }
#endif
    telemetry_.known_peers =
        static_cast<std::uint32_t>(remote_peers_.size());
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

    steam_id_by_role_.clear();
    steam_role_by_id_.clear();
    for (const steam::Peer& peer : steam::LobbyPeers()) {
      steam_id_by_role_[peer.role] = peer.steam_id;
      steam_role_by_id_[peer.steam_id] = peer.role;
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
    if (bound_role_ == 1) {
      const auto found = steam_role_by_id_.find(sender.steam_id);
      return found != steam_role_by_id_.end() &&
             found->second == sender_role;
    }
    const auto host = steam_id_by_role_.find(1);
    // Clients receive both the host's own packets and client packets relayed
    // by that host, so the authenticated transport sender must be the owner.
    return host != steam_id_by_role_.end() &&
           host->second == sender.steam_id;
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
                        offsetof(AnimationFragmentPacket, rows))) {
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

  void BeginRemoteSession(RemotePeerState& peer,
                          std::uint32_t sender_session) {
    if (peer.session == sender_session) {
      return;
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
    BeginRemoteSession(peer, packet.sender_session);
    if (!peer.samples.empty() &&
        (packet.sequence <= peer.samples.back().sequence ||
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
    const std::uint64_t receive_time_us = NowMicroseconds();
    const std::int64_t observed_clock_offset =
        static_cast<std::int64_t>(receive_time_us) -
        static_cast<std::int64_t>(packet.sender_time_us);
    if (!peer.clock_offset_valid) {
      peer.clock_offset_us = observed_clock_offset;
      peer.clock_offset_valid = true;
    } else if (observed_clock_offset < peer.clock_offset_us) {
      // A new minimum is the best available estimate of clock offset:
      // queueing can only make a packet arrive later, never earlier.
      peer.clock_offset_us = observed_clock_offset;
    } else {
      // Follow slow clock drift without letting one delayed packet shift
      // the entire presentation timeline.
      peer.clock_offset_us +=
          (observed_clock_offset - peer.clock_offset_us) / 128;
    }
    peer.samples.push_back(
        {now, packet.sender_time_us, pose, packet.sequence});
    while (peer.samples.size() > kMaximumBufferedSamples) {
      peer.samples.pop_front();
    }
    ++telemetry_.received_packets;
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
        offsetof(AnimationFragmentPacket, rows) +
        std::size_t(packet.float_count) * sizeof(std::uint16_t);
    const std::size_t total_floats =
        std::size_t(packet.bone_count) * 12;
    const std::size_t fragment_offset =
        std::size_t(packet.fragment_index) *
        kAnimationFragmentComponents;
    if (!CommonPacketValid(
            packet.version, packet.sender_role, packet.sender_session,
            packet.map_hash, map_hash) ||
        packet.byte_count != expected_bytes ||
        received_bytes != static_cast<int>(expected_bytes) ||
        packet.bone_count == 0 ||
        packet.bone_count > kMaximumAnimationBones ||
        packet.track_count == 0 ||
        packet.track_count > kMaximumAnimationTracks ||
        packet.track_index >= packet.track_count ||
        packet.fragment_count == 0 || packet.fragment_count > 8 ||
        packet.fragment_index >= packet.fragment_count ||
        packet.float_count == 0 ||
        packet.float_count > kAnimationFragmentComponents ||
        !Finite3(packet.root_position) ||
        fragment_offset + packet.float_count > total_floats) {
      ++telemetry_.rejected_packets;
      return false;
    }
    if (bound_role_ == 1 &&
        !IsHighDetailCached(packet.sender_role, 1)) {
      // The host can relay an already validated compact fragment without
      // allocating and decoding a skeleton it will not render locally.
      ++telemetry_.received_packets;
      return true;
    }
    RemotePeerState& peer = remote_peers_[packet.sender_role];
    BeginRemoteSession(peer, packet.sender_session);
    ++telemetry_.received_packets;
    if (!peer.animation_samples.empty() &&
        packet.sequence <=
            peer.animation_samples.back().pose.sequence) {
      return false;
    }
    if (peer.animation_assembly.session != packet.sender_session ||
        peer.animation_assembly.sequence != packet.sequence) {
      peer.animation_assembly = {};
      peer.animation_assembly.received_at = now;
      peer.animation_assembly.session = packet.sender_session;
      peer.animation_assembly.sequence = packet.sequence;
      peer.animation_assembly.sender_time_us = packet.sender_time_us;
      peer.animation_assembly.track_count = packet.track_count;
      peer.animation_assembly.tracks.resize(packet.track_count);
    }
    if (peer.animation_assembly.track_count != packet.track_count ||
        peer.animation_assembly.sender_time_us != packet.sender_time_us) {
      ++telemetry_.rejected_packets;
      peer.animation_assembly = {};
      return false;
    }
    AnimationTrackAssembly& track =
        peer.animation_assembly.tracks[packet.track_index];
    if (track.rows.empty()) {
      track.mesh_key = packet.mesh_key;
      track.bone_count = packet.bone_count;
      track.fragment_count = packet.fragment_count;
      track.rows.resize(total_floats);
    }
    if (track.mesh_key != packet.mesh_key ||
        track.bone_count != packet.bone_count ||
        track.fragment_count != packet.fragment_count) {
      ++telemetry_.rejected_packets;
      peer.animation_assembly = {};
      return false;
    }
    const std::uint64_t fragment_bit =
        std::uint64_t{1} << packet.fragment_index;
    if ((track.received_fragments & fragment_bit) == 0) {
      for (std::size_t index = 0; index < packet.float_count; ++index) {
        const std::size_t palette_index = fragment_offset + index;
        const std::size_t row_component = palette_index % 12;
        float value = 0.0f;
        if (row_component == 3 || row_component == 7 ||
            row_component == 11) {
          const auto quantized =
              static_cast<std::int16_t>(packet.rows[index]);
          value =
              packet.root_position[(row_component - 3) / 4] +
              static_cast<float>(quantized) /
                  kAnimationTranslationScale;
        } else {
          value = HalfToFloat(packet.rows[index]);
        }
        if (!std::isfinite(value)) {
          ++telemetry_.rejected_packets;
          peer.animation_assembly = {};
          return false;
        }
        track.rows[palette_index] = value;
      }
      track.received_fragments |= fragment_bit;
    }
    const std::uint64_t complete_mask =
        (std::uint64_t{1} << packet.fragment_count) - 1;
    if (track.received_fragments != complete_mask) {
      return true;
    }
    peer.animation_assembly.received_tracks |=
        std::uint64_t{1} << packet.track_index;
    const std::uint64_t complete_tracks =
        (std::uint64_t{1} << packet.track_count) - 1;
    if (peer.animation_assembly.received_tracks != complete_tracks) {
      return true;
    }
    ReceivedAnimationSample complete;
    complete.received_at = now;
    complete.pose.sender_time_us =
        peer.animation_assembly.sender_time_us;
    complete.pose.sequence = peer.animation_assembly.sequence;
    std::memcpy(
        complete.pose.root_position, packet.root_position,
        sizeof(complete.pose.root_position));
    complete.pose.tracks.reserve(
        peer.animation_assembly.tracks.size());
    std::uint32_t total_bones = 0;
    for (AnimationTrackAssembly& assembled :
         peer.animation_assembly.tracks) {
      complete.pose.tracks.push_back(
          {assembled.mesh_key, std::move(assembled.rows)});
      total_bones += assembled.bone_count;
    }
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
    if (role == 1 && local_position_valid_) {
      out = local_position_;
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
    const float radius = static_cast<float>(
        REXCVAR_GET(skate3_multiplayer_relevance_radius));
    if (source_distance_squared > radius * radius) {
      return false;
    }
    const std::size_t budget = static_cast<std::size_t>(
        std::max(
            REXCVAR_GET(skate3_multiplayer_relevance_players), 1));
    std::size_t closer = 0;
    if (source_role != 1 && local_position_valid_ &&
        distance_squared(local_position_, target_position) <
            source_distance_squared) {
      ++closer;
    }
    for (const auto& [candidate_role, candidate] : host_peers_) {
      if (candidate_role == source_role ||
          candidate_role == target_role ||
          !candidate.position_valid) {
        continue;
      }
      if (distance_squared(candidate.position, target_position) <
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
        REXCVAR_GET(skate3_multiplayer_far_presence_rate);
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

#if defined(_WIN32)
  bool SendBytes(const void* bytes, int byte_count,
                 const PacketEndpoint& target, bool relayed) {
    bool success = false;
    if (target.steam) {
      success = steam::SendPacketToPeer(
          target.steam_id, bytes,
          static_cast<std::size_t>(byte_count));
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
      if (bound_role_ != 1) {
        const auto host = steam_id_by_role_.find(1);
        if (host != steam_id_by_role_.end()) {
          PacketEndpoint target;
          target.steam = true;
          target.steam_id = host->second;
          targets.push_back({1, target});
        }
        return targets;
      }
      for (const auto& [target_role, steam_id] : steam_id_by_role_) {
        if (target_role == 1 ||
            target_role == static_cast<std::uint32_t>(bound_role_)) {
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
    const auto targets =
        LocalPacketTargets(source_role, animation, now);
    for (const auto& [target_role, endpoint] : targets) {
      if (target_role == source_role) {
        continue;
      }
      SendBytes(bytes, byte_count, endpoint, true);
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
          &packet, static_cast<int>(sizeof(packet)), target, false);
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
    const std::uint64_t sender_time_us = NowMicroseconds();
    (void)base_port;
    const auto targets = LocalPacketTargets(
        static_cast<std::uint32_t>(role), true, Clock::now());
    if (targets.empty()) {
      return;
    }
    bool complete = true;
    for (std::size_t track_index = 0;
         track_index < tracks.size(); ++track_index) {
      const AnimationTrack& track = *tracks[track_index];
      const std::size_t bone_count =
          std::min<std::size_t>(
              track.bone_rows.size() / 12, kMaximumAnimationBones);
      const std::size_t total_floats = bone_count * 12;
      const std::size_t fragment_count =
          (total_floats + kAnimationFragmentComponents - 1) /
          kAnimationFragmentComponents;
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
        packet.mesh_key = track.mesh_key;
        packet.bone_count = static_cast<std::uint16_t>(bone_count);
        packet.track_index =
            static_cast<std::uint16_t>(track_index);
        packet.track_count =
            static_cast<std::uint16_t>(tracks.size());
        packet.fragment_index =
            static_cast<std::uint16_t>(fragment_index);
        packet.fragment_count =
            static_cast<std::uint16_t>(fragment_count);
        const std::size_t offset =
            fragment_index * kAnimationFragmentComponents;
        packet.float_count = static_cast<std::uint16_t>(
            std::min<std::size_t>(
                kAnimationFragmentComponents, total_floats - offset));
        for (std::size_t index = 0; index < packet.float_count;
             ++index) {
          const std::size_t palette_index = offset + index;
          float value = track.bone_rows[palette_index];
          const std::size_t row_component = palette_index % 12;
          if (row_component == 3 || row_component == 7 ||
              row_component == 11) {
            const float relative =
                (value -
                 pose.root_position[(row_component - 3) / 4]) *
                kAnimationTranslationScale;
            const auto quantized = static_cast<std::int16_t>(
                std::clamp(
                    std::lround(relative), -32768l, 32767l));
            packet.rows[index] =
                static_cast<std::uint16_t>(quantized);
          } else {
            packet.rows[index] = FloatToHalf(value);
          }
        }
        packet.byte_count = static_cast<std::uint16_t>(
            offsetof(AnimationFragmentPacket, rows) +
            std::size_t(packet.float_count) *
                sizeof(std::uint16_t));
        for (const auto& [target_role, target] : targets) {
          (void)target_role;
          complete &=
              SendBytes(&packet, packet.byte_count, target, false);
        }
      }
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

  bool SmoothRemote(RemotePeerState& peer, Clock::time_point now,
                    RemotePose& out) {
    const auto interpolation_delay = std::chrono::milliseconds(
        REXCVAR_GET(skate3_multiplayer_local_interpolation_ms));
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
    const std::int64_t target_sender_time_us =
        local_now_us - peer.clock_offset_us -
        std::chrono::duration_cast<std::chrono::microseconds>(
            interpolation_delay)
            .count();
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
    out = peer.samples.back().pose;
    return true;
  }

  bool SmoothRemoteAnimation(RemotePeerState& peer,
                             Clock::time_point now,
                             const float map_origin[3],
                             AnimationPose& out) {
    const auto interpolation_delay = std::chrono::milliseconds(
        REXCVAR_GET(skate3_multiplayer_local_interpolation_ms));
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
    float amount = 0.0f;
    if (target_sender_time_us >=
        static_cast<std::int64_t>(
            peer.animation_samples.back().pose.sender_time_us)) {
      first = second = &peer.animation_samples.back().pose;
    } else {
      for (std::size_t index = 1;
           index < peer.animation_samples.size(); ++index) {
        const AnimationPose& candidate =
            peer.animation_samples[index].pose;
        if (target_sender_time_us <=
            static_cast<std::int64_t>(candidate.sender_time_us)) {
          first = &peer.animation_samples[index - 1].pose;
          second = &candidate;
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
          break;
        }
      }
    }
    if (second->tracks.empty()) {
      return false;
    }
    out.sender_time_us = second->sender_time_us;
    out.sequence = second->sequence;
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
      for (const AnimationTrack& candidate : first->tracks) {
        if (candidate.mesh_key == second_track.mesh_key &&
            candidate.bone_rows.size() ==
                second_track.bone_rows.size()) {
          first_track = &candidate;
          break;
        }
      }
      AnimationTrack output;
      output.mesh_key = second_track.mesh_key;
      output.bone_rows.resize(second_track.bone_rows.size());
      for (std::size_t index = 0;
           index < output.bone_rows.size(); ++index) {
        float value =
            first_track == nullptr
                ? second_track.bone_rows[index]
                : first_track->bone_rows[index] +
                      (second_track.bone_rows[index] -
                       first_track->bone_rows[index]) *
                          amount;
        const std::size_t row_component = index % 12;
        if (row_component == 3 || row_component == 7 ||
            row_component == 11) {
          value += map_origin[(row_component - 3) / 4];
        }
        output.bone_rows[index] = value;
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
    remote_peers_.clear();
#if defined(_WIN32)
    host_peers_.clear();
#endif
    far_presence_times_.clear();
    relevance_cache_.clear();
    std::fill_n(local_position_, 3, 0.0f);
    local_position_valid_ = false;
    last_send_ = {};
    last_animation_send_ = {};
    last_rate_log_ = {};
    last_rate_snapshot_ = {};
    telemetry_.socket_ready = false;
    telemetry_.session = 0;
    telemetry_.remote_animation_bones = 0;
    telemetry_.known_peers = 0;
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
  Clock::time_point last_send_{};
  Clock::time_point last_animation_send_{};
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
                      std::vector<RemotePlayer>& out_remotes) {
  return ActiveRuntime().Tick(
      map_name, map_render_origin, local_animation, out_remotes);
}

void AppendTelemetry(std::ostream& out) {
  ActiveRuntime().Append(out);
}

}  // namespace skate3::multiplayer
