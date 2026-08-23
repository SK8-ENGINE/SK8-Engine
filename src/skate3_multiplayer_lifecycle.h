#pragma once

#include "skate3_multiplayer_protocol.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace skate3::multiplayer::lifecycle {

inline constexpr std::size_t kMaximumIncompleteAppearanceBytes =
    64 * 1024 * 1024;
inline constexpr auto kAppearanceAssemblyIdleTimeout = std::chrono::seconds(10);
inline constexpr auto kAppearanceResendRequestMinimumInterval =
    std::chrono::seconds(2);

// Renderer texture entries are mutable GPU resources. Scope their content
// identity by role so two players wearing the same recipe cannot replace or
// retire each other's texture-store entry.
[[nodiscard]] constexpr std::uint64_t RemoteAppearanceTextureStoreKey(
    std::uint32_t role, std::uint64_t content_key) {
  if (role < 1 || role > 100 || content_key == 0) {
    return 0;
  }
  constexpr std::uint64_t kFnvPrime = 1099511628211ull;
  std::uint64_t scoped_key = content_key;
  for (std::size_t byte = 0; byte < sizeof(role); ++byte) {
    scoped_key =
        (scoped_key ^
         ((static_cast<std::uint64_t>(role) >> (byte * 8)) & 0xFFu)) *
        kFnvPrime;
  }
  return scoped_key;
}

[[nodiscard]] constexpr bool RemoteAppearanceSessionChanged(
    std::uint32_t installed_session, std::uint32_t observed_session) {
  return installed_session != 0 && observed_session != 0 &&
         installed_session != observed_session;
}

[[nodiscard]] constexpr bool RemoteAppearanceRetirementMatches(
    std::uint32_t installed_session, std::uint32_t retired_session) {
  return installed_session != 0 &&
         installed_session == retired_session;
}

[[nodiscard]] constexpr bool
CanBeginAppearanceAssembly(std::size_t other_incomplete_bytes,
                           std::size_t requested_bytes) {
  return requested_bytes > 0 &&
         requested_bytes <= protocol::kMaximumAppearanceBytes &&
         other_incomplete_bytes <= kMaximumIncompleteAppearanceBytes &&
         requested_bytes <=
             kMaximumIncompleteAppearanceBytes - other_incomplete_bytes;
}

[[nodiscard]] constexpr bool
CompleteAppearancePieceCount(std::size_t expected,
                             std::size_t actual) {
  return expected > 0 && actual == expected;
}

template <typename Clock>
[[nodiscard]] bool
AppearanceAssemblyExpired(typename Clock::time_point now,
                          typename Clock::time_point last_update) {
  return last_update != typename Clock::time_point{} &&
         now - last_update > kAppearanceAssemblyIdleTimeout;
}

// Appearance bulk data uses a direct per-recipient stream on Steam. The
// localhost fallback gives non-host clients only one upstream endpoint, so
// their requested resend must restart the stream to role 1 for relay.
[[nodiscard]] constexpr std::uint32_t AppearanceResendTargetRole(
    std::uint32_t local_role, bool using_steam,
    std::uint32_t requester_role) {
  if (local_role < 1 || local_role > 100 ||
      requester_role < 1 || requester_role > 100 ||
      local_role == requester_role) {
    return 0;
  }
  return using_steam || local_role == 1 ? requester_role : 1;
}

struct OutboundAppearanceState {
  std::uint64_t identity = 0;
  std::uint16_t next_chunk = 0;
  std::uint8_t completed_passes = 0;
  std::chrono::steady_clock::time_point retry_after{};
  protocol::AppearanceDeliveryState acknowledged_state =
      protocol::AppearanceDeliveryState::kUnknown;

  void Reset(std::uint64_t new_identity) {
    *this = {};
    identity = new_identity;
  }

  [[nodiscard]] bool RestartForRequest(
      std::uint64_t requested_identity,
      std::uint64_t current_identity) {
    if (requested_identity == 0 ||
        requested_identity != current_identity) {
      return false;
    }
    Reset(current_identity);
    return true;
  }
};

// Roles are reusable presentation slots, not durable peer identities. Track
// both the authenticated transport identity and the sender's process session
// so outbound delta/appearance state can be discarded before a new occupant
// inherits it.
class PeerGenerationTracker {
public:
  [[nodiscard]] bool ObserveTransportIdentity(std::uint32_t role,
                                              std::uint64_t identity) {
    if (!ValidRole(role) || identity == 0) {
      return false;
    }
    PeerGeneration &peer = peers_[role];
    if (peer.transport_identity == identity) {
      return false;
    }
    peer.transport_identity = identity;
    // A new authenticated transport occupant cannot inherit a process
    // session observed from the previous occupant of this role.
    peer.process_session = 0;
    return true;
  }

  [[nodiscard]] bool ObserveProcessSession(std::uint32_t role,
                                           std::uint32_t session) {
    if (!ValidRole(role) || session == 0) {
      return false;
    }
    PeerGeneration &peer = peers_[role];
    if (peer.process_session == session) {
      return false;
    }
    peer.process_session = session;
    return true;
  }

  [[nodiscard]] bool Forget(std::uint32_t role) {
    return peers_.erase(role) != 0;
  }

  void Clear() { peers_.clear(); }

  [[nodiscard]] std::size_t size() const { return peers_.size(); }

private:
  struct PeerGeneration {
    std::uint64_t transport_identity = 0;
    std::uint32_t process_session = 0;
  };

  [[nodiscard]] static constexpr bool ValidRole(std::uint32_t role) {
    return role >= 1 && role <= 100;
  }

  std::unordered_map<std::uint32_t, PeerGeneration> peers_;
};

} // namespace skate3::multiplayer::lifecycle
