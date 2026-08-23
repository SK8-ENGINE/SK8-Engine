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

[[nodiscard]] constexpr bool
CanBeginAppearanceAssembly(std::size_t other_incomplete_bytes,
                           std::size_t requested_bytes) {
  return requested_bytes > 0 &&
         requested_bytes <= protocol::kMaximumAppearanceBytes &&
         other_incomplete_bytes <= kMaximumIncompleteAppearanceBytes &&
         requested_bytes <=
             kMaximumIncompleteAppearanceBytes - other_incomplete_bytes;
}

template <typename Clock>
[[nodiscard]] bool
AppearanceAssemblyExpired(typename Clock::time_point now,
                          typename Clock::time_point last_update) {
  return last_update != typename Clock::time_point{} &&
         now - last_update > kAppearanceAssemblyIdleTimeout;
}

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
