#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <vector>

namespace skate3::multiplayer::player_collision {

// Skate 3 map and board transforms use metres, with Y as the vertical axis.
// A 30 cm body radius keeps shoulder-to-shoulder contact close to the rendered
// skater instead of giving each player a metre-wide personal space bubble.
inline constexpr float kPlayerProxyRadius = 0.30f;
inline constexpr float kPlayerProxyLowerCenter = -0.20f;
inline constexpr float kPlayerProxyUpperCenter = 1.25f;
inline constexpr std::uint32_t kPlayerProxyRadialSegments = 12;
inline constexpr std::uint32_t kPlayerProxyHemisphereSegments = 3;

inline constexpr std::uint64_t kRemoteSampleStaleUs = 1'500'000;
inline constexpr float kFacingTestSpawnSpacing = 8.0f;

enum class DisabledReason : std::uint8_t {
  kNone = 0,
  kMultiplayerInactive,
  kInvalidLocalIdentity,
  kInvalidWorld,
  kPresentationStale,
  kMenu,
  kMapEditor,
  kLocalNotPlaying,
  kGuestStateInvalid,
};

struct RemoteProxySample {
  std::uint32_t role = 0;
  std::uint32_t session = 0;
  std::uint32_t map_hash = 0;
  std::array<float, 3> position{};
  std::uint64_t observed_at_us = 0;
  bool spatial_valid = false;
  bool playing = false;
};

struct UpdateContext {
  bool enabled = false;
  std::uint32_t local_role = 0;
  std::uint32_t local_session = 0;
  std::uint32_t map_hash = 0;
  std::uint64_t now_us = 0;
  DisabledReason disabled_reason = DisabledReason::kMultiplayerInactive;
};

struct Proxy {
  std::uint32_t role = 0;
  std::uint32_t session = 0;
  std::uint32_t map_hash = 0;
  std::array<float, 3> position{};
};

struct Counters {
  std::uint64_t created = 0;
  std::uint64_t updated = 0;
  std::uint64_t removed = 0;
  std::uint64_t stale_cleanups = 0;
};

struct CapsuleTriangle {
  std::array<float, 3> a{};
  std::array<float, 3> b{};
  std::array<float, 3> c{};
  std::array<float, 3> normal{};
};

struct FacingTestSpawn {
  std::array<float, 16> transform{};
  bool valid = false;
};

[[nodiscard]] inline std::vector<CapsuleTriangle>
BuildPlayerCapsuleTriangles() {
  struct Ring {
    float y = 0.0f;
    float radius = 0.0f;
  };

  constexpr float kPi = 3.14159265358979323846f;
  std::vector<Ring> rings;
  rings.reserve(kPlayerProxyHemisphereSegments * 2 + 2);
  rings.push_back(
      {kPlayerProxyLowerCenter - kPlayerProxyRadius, 0.0f});
  for (std::uint32_t segment = 1;
       segment <= kPlayerProxyHemisphereSegments; ++segment) {
    const float angle =
        -kPi * 0.5f +
        kPi * 0.5f * static_cast<float>(segment) /
            static_cast<float>(kPlayerProxyHemisphereSegments);
    rings.push_back(
        {kPlayerProxyLowerCenter + kPlayerProxyRadius * std::sin(angle),
         kPlayerProxyRadius * std::cos(angle)});
  }
  rings.push_back({kPlayerProxyUpperCenter, kPlayerProxyRadius});
  for (std::uint32_t segment = 1;
       segment <= kPlayerProxyHemisphereSegments; ++segment) {
    const float angle =
        kPi * 0.5f * static_cast<float>(segment) /
        static_cast<float>(kPlayerProxyHemisphereSegments);
    rings.push_back(
        {kPlayerProxyUpperCenter + kPlayerProxyRadius * std::sin(angle),
         kPlayerProxyRadius * std::cos(angle)});
  }

  const auto point = [](const Ring &ring, std::uint32_t segment) {
    constexpr float kTwoPi = 6.28318530717958647692f;
    const float angle =
        kTwoPi * static_cast<float>(segment) /
        static_cast<float>(kPlayerProxyRadialSegments);
    return std::array<float, 3>{
        ring.radius * std::cos(angle), ring.y,
        ring.radius * std::sin(angle)};
  };
  const auto triangle =
      [](const std::array<float, 3> &a, const std::array<float, 3> &b,
         const std::array<float, 3> &c) {
        const std::array<float, 3> ab{
            b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const std::array<float, 3> ac{
            c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        std::array<float, 3> normal{
            ab[1] * ac[2] - ab[2] * ac[1],
            ab[2] * ac[0] - ab[0] * ac[2],
            ab[0] * ac[1] - ab[1] * ac[0]};
        const float length = std::sqrt(
            normal[0] * normal[0] + normal[1] * normal[1] +
            normal[2] * normal[2]);
        if (length > 0.0f) {
          normal[0] /= length;
          normal[1] /= length;
          normal[2] /= length;
        }
        return CapsuleTriangle{a, b, c, normal};
      };

  std::vector<CapsuleTriangle> triangles;
  triangles.reserve(
      kPlayerProxyRadialSegments *
      (kPlayerProxyHemisphereSegments * 4 + 2));
  for (std::size_t ring = 0; ring + 1 < rings.size(); ++ring) {
    const bool lower_pole = rings[ring].radius < 1.0e-6f;
    const bool upper_pole = rings[ring + 1].radius < 1.0e-6f;
    for (std::uint32_t segment = 0;
         segment < kPlayerProxyRadialSegments; ++segment) {
      const std::uint32_t next =
          (segment + 1) % kPlayerProxyRadialSegments;
      const auto lower = point(rings[ring], segment);
      const auto lower_next = point(rings[ring], next);
      const auto upper = point(rings[ring + 1], segment);
      const auto upper_next = point(rings[ring + 1], next);
      if (lower_pole) {
        triangles.push_back(triangle(lower, upper, upper_next));
      } else if (upper_pole) {
        triangles.push_back(triangle(lower, upper, lower_next));
      } else {
        triangles.push_back(triangle(lower, upper, upper_next));
        triangles.push_back(triangle(lower, upper_next, lower_next));
      }
    }
  }
  return triangles;
}

[[nodiscard]] inline FacingTestSpawn
BuildFacingTestSpawn(std::uint32_t local_role,
                     const std::array<float, 16> &source) {
  FacingTestSpawn result;
  result.transform = source;
  if (local_role < 1 || local_role > 2) {
    return result;
  }
  for (const float component : source) {
    if (!std::isfinite(component)) {
      return result;
    }
  }

  float heading_x = source[8];
  float heading_z = source[10];
  float heading_length =
      std::sqrt(heading_x * heading_x + heading_z * heading_z);
  if (heading_length < 1.0e-5f) {
    // A nearly vertical forward axis is not a useful skating direction.
    // Recover a horizontal forward heading from the board's right axis.
    heading_x = -source[2];
    heading_z = source[0];
    heading_length = std::sqrt(heading_x * heading_x + heading_z * heading_z);
  }
  if (heading_length < 1.0e-5f) {
    return result;
  }
  heading_x /= heading_length;
  heading_z /= heading_length;

  const float signed_half_spacing = local_role == 1
                                        ? -kFacingTestSpawnSpacing * 0.5f
                                        : kFacingTestSpawnSpacing * 0.5f;
  result.transform[12] += heading_x * signed_half_spacing;
  result.transform[14] += heading_z * signed_half_spacing;
  if (local_role == 2) {
    for (std::size_t component = 0; component < 3; ++component) {
      result.transform[component] = -result.transform[component];
      result.transform[8 + component] = -result.transform[8 + component];
    }
  }
  result.valid = true;
  return result;
}

class ProxySet {
public:
  void Update(const UpdateContext &context,
              std::span<const RemoteProxySample> samples) {
    if (!context.enabled || context.local_role < 1 ||
        context.local_role > 100 || context.local_session == 0 ||
        context.map_hash == 0 || context.now_us == 0) {
      const DisabledReason reason = context.enabled
                                        ? DisabledReason::kInvalidLocalIdentity
                                        : context.disabled_reason;
      Clear(reason, reason == DisabledReason::kPresentationStale);
      local_role_ = 0;
      local_session_ = 0;
      map_hash_ = 0;
      return;
    }

    if ((local_session_ != 0 && local_session_ != context.local_session) ||
        (map_hash_ != 0 && map_hash_ != context.map_hash) ||
        (local_role_ != 0 && local_role_ != context.local_role)) {
      Clear(DisabledReason::kInvalidWorld, false);
    }
    local_role_ = context.local_role;
    local_session_ = context.local_session;
    map_hash_ = context.map_hash;
    disabled_reason_ = DisabledReason::kNone;

    std::set<std::uint32_t> retained_roles;
    std::set<std::uint32_t> stale_roles;
    for (const RemoteProxySample &sample : samples) {
      const bool identity_valid = sample.role >= 1 && sample.role <= 100 &&
                                  sample.role != context.local_role &&
                                  sample.session != 0 &&
                                  sample.map_hash == context.map_hash;
      const bool time_valid =
          sample.observed_at_us != 0 &&
          sample.observed_at_us <= context.now_us + 100'000 &&
          context.now_us - std::min(context.now_us, sample.observed_at_us) <=
              kRemoteSampleStaleUs;
      if (!identity_valid || !sample.spatial_valid || !sample.playing ||
          !Finite(sample.position) || !time_valid) {
        if (identity_valid && !time_valid) {
          stale_roles.insert(sample.role);
        }
        continue;
      }
      if (!retained_roles.insert(sample.role).second) {
        continue;
      }

      auto existing = proxies_.find(sample.role);
      if (existing != proxies_.end() &&
          existing->second.session != sample.session) {
        proxies_.erase(existing);
        ++counters_.removed;
        existing = proxies_.end();
      }
      if (existing == proxies_.end()) {
        Proxy proxy;
        proxy.role = sample.role;
        proxy.session = sample.session;
        proxy.map_hash = sample.map_hash;
        proxy.position = sample.position;
        proxies_.emplace(sample.role, proxy);
        ++counters_.created;
        continue;
      }

      Proxy &proxy = existing->second;
      proxy.position = sample.position;
      ++counters_.updated;
    }

    for (auto iterator = proxies_.begin(); iterator != proxies_.end();) {
      if (!retained_roles.contains(iterator->first)) {
        if (stale_roles.contains(iterator->first)) {
          ++counters_.stale_cleanups;
        }
        ++counters_.removed;
        iterator = proxies_.erase(iterator);
      } else {
        ++iterator;
      }
    }
  }

  void Disable(DisabledReason reason, bool stale_cleanup = false) {
    Clear(reason, stale_cleanup);
  }

  [[nodiscard]] std::size_t size() const { return proxies_.size(); }
  [[nodiscard]] DisabledReason disabled_reason() const {
    return disabled_reason_;
  }
  [[nodiscard]] const Counters &counters() const { return counters_; }
  [[nodiscard]] const Proxy *Find(std::uint32_t role) const {
    const auto found = proxies_.find(role);
    return found == proxies_.end() ? nullptr : &found->second;
  }
  [[nodiscard]] const std::map<std::uint32_t, Proxy> &proxies() const {
    return proxies_;
  }

private:
  [[nodiscard]] static bool Finite(const std::array<float, 3> &value) {
    return std::isfinite(value[0]) && std::isfinite(value[1]) &&
           std::isfinite(value[2]);
  }

  void Clear(DisabledReason reason, bool stale_cleanup) {
    if (stale_cleanup) {
      counters_.stale_cleanups += proxies_.size();
    }
    counters_.removed += proxies_.size();
    proxies_.clear();
    disabled_reason_ = reason;
  }

  std::map<std::uint32_t, Proxy> proxies_;
  std::uint32_t local_role_ = 0;
  std::uint32_t local_session_ = 0;
  std::uint32_t map_hash_ = 0;
  DisabledReason disabled_reason_ = DisabledReason::kMultiplayerInactive;
  Counters counters_;
};

} // namespace skate3::multiplayer::player_collision
