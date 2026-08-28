#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <span>

namespace skate3::multiplayer::player_collision {

// Match the established project-owned player capsule. Skate 3 map and board
// transforms use metres, with Y as the vertical axis.
inline constexpr float kPlayerProxyRadius = 0.52f;
inline constexpr float kPlayerProxyLowerCenter = -0.20f;
inline constexpr float kPlayerProxyUpperCenter = 1.25f;

inline constexpr std::uint32_t kLocalPlayerLayer = 1u << 8;
inline constexpr std::uint32_t kRemotePlayerLayer = 1u << 9;
inline constexpr std::uint32_t kLocalPlayerMask = kRemotePlayerLayer;
inline constexpr std::uint32_t kRemotePlayerMask = kLocalPlayerLayer;

inline constexpr std::uint64_t kRemoteSampleStaleUs = 1'500'000;
inline constexpr std::uint64_t kOverlapGraceUs = 350'000;
inline constexpr float kTeleportDistance = 3.0f;
inline constexpr float kContactSlop = 0.02f;
inline constexpr float kMaximumRemoteSpeed = 20.0f;
inline constexpr float kMaximumCorrectionSpeed = 6.0f;
inline constexpr float kMaximumTotalCorrectionSpeed = 8.0f;
inline constexpr float kGraceCorrectionSpeed = 0.75f;
inline constexpr float kMaximumEquivalentImpulse = 450.0f;

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
  bool discontinuity = false;
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

struct LocalSample {
  std::array<float, 3> position{};
  std::uint64_t observed_at_us = 0;
  float step_seconds = 1.0f / 60.0f;
};

struct Proxy {
  std::uint32_t role = 0;
  std::uint32_t session = 0;
  std::uint32_t map_hash = 0;
  std::uint32_t layer = kRemotePlayerLayer;
  std::uint32_t mask = kRemotePlayerMask;
  std::array<float, 3> position{};
  std::array<float, 3> velocity{};
  std::uint64_t last_observed_us = 0;
  std::uint64_t grace_until_us = 0;
  bool discontinuity_active = false;
};

struct Counters {
  std::uint64_t created = 0;
  std::uint64_t updated = 0;
  std::uint64_t removed = 0;
  std::uint64_t stale_cleanups = 0;
  std::uint64_t teleports = 0;
  std::uint64_t contacts = 0;
  std::uint64_t grace_contacts = 0;
  float maximum_correction = 0.0f;
  float maximum_equivalent_impulse = 0.0f;
};

struct ResolveResult {
  std::array<float, 3> corrected_position{};
  std::array<float, 3> correction{};
  std::uint32_t contacts = 0;
  bool grace_contact = false;
  float maximum_contact_correction = 0.0f;
  float maximum_equivalent_impulse = 0.0f;
};

[[nodiscard]] inline bool LayersCollide(std::uint32_t first_layer,
                                        std::uint32_t first_mask,
                                        std::uint32_t second_layer,
                                        std::uint32_t second_mask) {
  return (first_mask & second_layer) != 0 && (second_mask & first_layer) != 0;
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
      ResetLocalHistory();
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
        proxy.last_observed_us = sample.observed_at_us;
        proxy.grace_until_us = context.now_us + kOverlapGraceUs;
        proxies_.emplace(sample.role, proxy);
        ++counters_.created;
        continue;
      }

      Proxy &proxy = existing->second;
      const std::array<float, 3> delta =
          Subtract(sample.position, proxy.position);
      const float distance_squared = LengthSquared(delta);
      const bool teleported =
          (sample.discontinuity && !proxy.discontinuity_active) ||
          distance_squared > kTeleportDistance * kTeleportDistance;
      if (sample.discontinuity ||
          distance_squared > kTeleportDistance * kTeleportDistance) {
        proxy.velocity = {};
        proxy.grace_until_us = context.now_us + kOverlapGraceUs;
        if (teleported) {
          ++counters_.teleports;
        }
      } else if (sample.observed_at_us > proxy.last_observed_us) {
        const float elapsed_seconds =
            static_cast<float>(sample.observed_at_us - proxy.last_observed_us) *
            1.0e-6f;
        if (elapsed_seconds > 0.0f && elapsed_seconds <= 0.5f) {
          proxy.velocity = Scale(delta, 1.0f / elapsed_seconds);
          ClampMagnitude(proxy.velocity, kMaximumRemoteSpeed);
        }
      }
      proxy.position = sample.position;
      proxy.last_observed_us = sample.observed_at_us;
      proxy.discontinuity_active = sample.discontinuity;
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

  ResolveResult Resolve(const LocalSample &local) {
    ResolveResult result;
    result.corrected_position = local.position;
    if (disabled_reason_ != DisabledReason::kNone || !Finite(local.position) ||
        local.observed_at_us == 0) {
      return result;
    }

    const float step_seconds =
        std::clamp(local.step_seconds, 1.0f / 240.0f, 1.0f / 30.0f);
    std::array<float, 3> local_velocity{};
    if (local_history_valid_ && local.observed_at_us > local_observed_at_us_) {
      const std::array<float, 3> movement =
          Subtract(local.position, local_position_);
      const float elapsed_seconds =
          static_cast<float>(local.observed_at_us - local_observed_at_us_) *
          1.0e-6f;
      if (LengthSquared(movement) > kTeleportDistance * kTeleportDistance) {
        local_grace_until_us_ = local.observed_at_us + kOverlapGraceUs;
      } else if (elapsed_seconds > 0.0f && elapsed_seconds <= 0.5f) {
        local_velocity = Scale(movement, 1.0f / elapsed_seconds);
        ClampMagnitude(local_velocity, kMaximumRemoteSpeed);
      }
    } else if (!local_history_valid_) {
      local_grace_until_us_ = local.observed_at_us + kOverlapGraceUs;
    }
    local_position_ = local.position;
    local_observed_at_us_ = local.observed_at_us;
    local_history_valid_ = true;

    const float total_limit = kMaximumTotalCorrectionSpeed * step_seconds;
    for (const auto &[role, proxy] : proxies_) {
      if (!LayersCollide(kLocalPlayerLayer, kLocalPlayerMask, proxy.layer,
                         proxy.mask)) {
        continue;
      }

      const float local_lower =
          result.corrected_position[1] + kPlayerProxyLowerCenter;
      const float local_upper =
          result.corrected_position[1] + kPlayerProxyUpperCenter;
      const float remote_lower = proxy.position[1] + kPlayerProxyLowerCenter;
      const float remote_upper = proxy.position[1] + kPlayerProxyUpperCenter;
      const float vertical_gap = std::max(
          {remote_lower - local_upper, local_lower - remote_upper, 0.0f});
      const float combined_radius = kPlayerProxyRadius * 2.0f;
      if (vertical_gap >= combined_radius) {
        continue;
      }
      const float horizontal_contact_radius =
          std::sqrt(std::max(0.0f, combined_radius * combined_radius -
                                       vertical_gap * vertical_gap));
      float delta_x = result.corrected_position[0] - proxy.position[0];
      float delta_z = result.corrected_position[2] - proxy.position[2];
      const float distance_squared = delta_x * delta_x + delta_z * delta_z;
      const float distance = std::sqrt(distance_squared);
      const float penetration =
          horizontal_contact_radius - distance - kContactSlop;
      if (penetration <= 0.0f) {
        continue;
      }

      float normal_x = 0.0f;
      float normal_z = 0.0f;
      if (distance > 1.0e-5f) {
        normal_x = delta_x / distance;
        normal_z = delta_z / distance;
      } else {
        DeterministicFallbackNormal(local_role_, role, normal_x, normal_z);
      }

      const bool grace = local.observed_at_us < local_grace_until_us_ ||
                         local.observed_at_us < proxy.grace_until_us;
      const float relative_x = local_velocity[0] - proxy.velocity[0];
      const float relative_z = local_velocity[2] - proxy.velocity[2];
      const float closing_speed =
          std::max(0.0f, -(relative_x * normal_x + relative_z * normal_z));
      const float speed_limit =
          grace ? kGraceCorrectionSpeed : kMaximumCorrectionSpeed;
      const float contact_limit = speed_limit * step_seconds;
      const float velocity_response =
          grace ? 0.0f : closing_speed * step_seconds * 0.35f;
      float correction =
          std::min(penetration * 0.55f + velocity_response, contact_limit);

      const float used_total =
          std::sqrt(result.correction[0] * result.correction[0] +
                    result.correction[2] * result.correction[2]);
      correction =
          std::min(correction, std::max(0.0f, total_limit - used_total));
      if (correction <= 0.0f) {
        continue;
      }

      const float correction_x = normal_x * correction;
      const float correction_z = normal_z * correction;
      result.corrected_position[0] += correction_x;
      result.corrected_position[2] += correction_z;
      result.correction[0] += correction_x;
      result.correction[2] += correction_z;
      ++result.contacts;
      result.grace_contact = result.grace_contact || grace;
      result.maximum_contact_correction =
          std::max(result.maximum_contact_correction, correction);
      const float equivalent_impulse =
          grace ? 0.0f
                : std::min(kMaximumEquivalentImpulse, closing_speed * 37.5f);
      result.maximum_equivalent_impulse =
          std::max(result.maximum_equivalent_impulse, equivalent_impulse);
    }

    counters_.contacts += result.contacts;
    if (result.grace_contact) {
      ++counters_.grace_contacts;
    }
    counters_.maximum_correction = std::max(counters_.maximum_correction,
                                            result.maximum_contact_correction);
    counters_.maximum_equivalent_impulse =
        std::max(counters_.maximum_equivalent_impulse,
                 result.maximum_equivalent_impulse);
    return result;
  }

  void Disable(DisabledReason reason, bool stale_cleanup = false) {
    Clear(reason, stale_cleanup);
    ResetLocalHistory();
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

private:
  [[nodiscard]] static bool Finite(const std::array<float, 3> &value) {
    return std::isfinite(value[0]) && std::isfinite(value[1]) &&
           std::isfinite(value[2]);
  }

  [[nodiscard]] static std::array<float, 3>
  Subtract(const std::array<float, 3> &left,
           const std::array<float, 3> &right) {
    return {left[0] - right[0], left[1] - right[1], left[2] - right[2]};
  }

  [[nodiscard]] static std::array<float, 3>
  Scale(const std::array<float, 3> &value, float amount) {
    return {value[0] * amount, value[1] * amount, value[2] * amount};
  }

  [[nodiscard]] static float LengthSquared(const std::array<float, 3> &value) {
    return value[0] * value[0] + value[1] * value[1] + value[2] * value[2];
  }

  static void ClampMagnitude(std::array<float, 3> &value, float maximum) {
    const float length_squared = LengthSquared(value);
    if (length_squared <= maximum * maximum || length_squared <= 0.0f) {
      return;
    }
    const float scale = maximum / std::sqrt(length_squared);
    value = Scale(value, scale);
  }

  static void DeterministicFallbackNormal(std::uint32_t local_role,
                                          std::uint32_t remote_role,
                                          float &out_x, float &out_z) {
    const std::uint32_t pair_key =
        std::min(local_role, remote_role) + std::max(local_role, remote_role);
    const float direction = local_role < remote_role ? -1.0f : 1.0f;
    if ((pair_key & 1u) == 0) {
      out_x = direction;
      out_z = 0.0f;
    } else {
      out_x = 0.0f;
      out_z = direction;
    }
  }

  void Clear(DisabledReason reason, bool stale_cleanup) {
    if (stale_cleanup) {
      counters_.stale_cleanups += proxies_.size();
    }
    counters_.removed += proxies_.size();
    proxies_.clear();
    disabled_reason_ = reason;
  }

  void ResetLocalHistory() {
    local_history_valid_ = false;
    local_position_ = {};
    local_observed_at_us_ = 0;
    local_grace_until_us_ = 0;
  }

  std::map<std::uint32_t, Proxy> proxies_;
  std::uint32_t local_role_ = 0;
  std::uint32_t local_session_ = 0;
  std::uint32_t map_hash_ = 0;
  DisabledReason disabled_reason_ = DisabledReason::kMultiplayerInactive;
  bool local_history_valid_ = false;
  std::array<float, 3> local_position_{};
  std::uint64_t local_observed_at_us_ = 0;
  std::uint64_t local_grace_until_us_ = 0;
  Counters counters_;
};

} // namespace skate3::multiplayer::player_collision
