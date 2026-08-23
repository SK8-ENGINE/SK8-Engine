#pragma once

#include "skate3_multiplayer_outbound_scheduler.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace skate3::multiplayer {

// Replication addresses peers by authenticated connection identity. Transport
// adapters own the platform-specific address (sockaddr, Steam identity, or
// server connection) behind connection_id.
enum class TransportKind : std::uint8_t {
  kLocalhostUdp,
  kSteamMessages,
  kSteamSockets,
  kDedicatedServer,
};

struct TransportEndpoint {
  TransportKind kind = TransportKind::kLocalhostUdp;
  std::uint64_t connection_id = 0;
  std::uint32_t role = 0;
  std::uint64_t generation = 0;

  [[nodiscard]] constexpr bool Valid() const {
    return connection_id != 0 && role >= 1 && role <= 100 &&
           generation != 0;
  }

  [[nodiscard]] constexpr bool
  operator==(const TransportEndpoint&) const = default;
};

struct TransportDatagramView {
  TransportEndpoint target;
  OutboundTrafficClass traffic_class =
      OutboundTrafficClass::kRealtime;
  std::uint64_t expires_at_us = 0;
  std::span<const std::uint8_t> bytes;

  [[nodiscard]] constexpr bool Valid(std::uint64_t now_us) const {
    if (!target.Valid() || bytes.empty() ||
        bytes.size() > protocol_v12::kMaximumDatagramBytes) {
      return false;
    }
    if (traffic_class == OutboundTrafficClass::kRealtime) {
      return expires_at_us > now_us;
    }
    return expires_at_us == 0;
  }
};

struct TransportReceivedDatagram {
  TransportEndpoint source;
  std::uint64_t received_at_us = 0;
  std::vector<std::uint8_t> bytes;
};

struct TransportBatchResult {
  std::size_t accepted_datagrams = 0;
  std::size_t accepted_bytes = 0;
  std::size_t rejected_datagrams = 0;
  std::size_t rejected_bytes = 0;
};

struct TransportQueueSnapshot {
  std::uint64_t captured_at_us = 0;
  std::size_t pending_control_bytes = 0;
  std::size_t pending_realtime_bytes = 0;
  std::size_t pending_appearance_bytes = 0;
  std::uint64_t sent_datagrams = 0;
  std::uint64_t sent_bytes = 0;
  std::uint64_t send_failures = 0;
  std::uint64_t expired_realtime_datagrams = 0;
  std::uint32_t estimated_rtt_ms = 0;
  float local_quality = -1.0f;
  float remote_quality = -1.0f;
};

// The replication worker depends only on this interface. UDP, current Steam
// Messages, Steam Networking Sockets, and a future server connection can
// therefore share packet formats, class scheduling, recovery, and playback.
class TransportAdapter {
 public:
  virtual ~TransportAdapter() = default;

  [[nodiscard]] virtual TransportKind kind() const = 0;
  virtual TransportBatchResult SendBatch(
      std::span<const TransportDatagramView> datagrams,
      std::uint64_t now_us) = 0;
  virtual std::vector<TransportReceivedDatagram> ReceiveBatch(
      std::size_t maximum_datagrams, std::uint64_t now_us) = 0;
  [[nodiscard]] virtual TransportQueueSnapshot QueueSnapshot(
      std::uint64_t now_us) const = 0;
};

}  // namespace skate3::multiplayer
