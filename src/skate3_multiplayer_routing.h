#pragma once

#include "skate3_multiplayer_protocol_v12.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace skate3::multiplayer::routing {

enum class RouteMode : std::uint8_t {
  kDirectPeerMesh,
  kDedicatedRelay,
};

struct RouteRecommendation {
  RouteMode mode = RouteMode::kDirectPeerMesh;
  bool dedicated_preferred = false;
  bool supported = false;
};

[[nodiscard]] constexpr RouteRecommendation RecommendRoute(
    std::uint32_t participant_count, bool dedicated_available) {
  if (participant_count < 2 || participant_count > 100) {
    return {};
  }
  if (participant_count <= 20 || !dedicated_available) {
    return {
        .mode = RouteMode::kDirectPeerMesh,
        .dedicated_preferred = participant_count > 20,
        .supported = true,
    };
  }
  return {
      .mode = RouteMode::kDedicatedRelay,
      .dedicated_preferred = true,
      .supported = true,
  };
}

struct BandwidthProjection {
  double client_upload_bytes_per_second = 0.0;
  double client_download_bytes_per_second = 0.0;
  double relay_ingress_bytes_per_second = 0.0;
  double relay_egress_bytes_per_second = 0.0;
  double aggregate_bytes_per_second = 0.0;
};

[[nodiscard]] constexpr BandwidthProjection ProjectBandwidth(
    std::uint32_t participant_count, double stream_bytes_per_second,
    RouteMode mode) {
  if (participant_count < 2 || participant_count > 100 ||
      !(stream_bytes_per_second >= 0.0)) {
    return {};
  }
  const double peers = static_cast<double>(participant_count - 1);
  const double players = static_cast<double>(participant_count);
  if (mode == RouteMode::kDirectPeerMesh) {
    return {
        .client_upload_bytes_per_second = stream_bytes_per_second * peers,
        .client_download_bytes_per_second = stream_bytes_per_second * peers,
        .relay_ingress_bytes_per_second = 0.0,
        .relay_egress_bytes_per_second = 0.0,
        .aggregate_bytes_per_second =
            stream_bytes_per_second * players * peers,
    };
  }
  return {
      .client_upload_bytes_per_second = stream_bytes_per_second,
      .client_download_bytes_per_second = stream_bytes_per_second * peers,
      .relay_ingress_bytes_per_second = stream_bytes_per_second * players,
      .relay_egress_bytes_per_second =
          stream_bytes_per_second * players * peers,
      .aggregate_bytes_per_second =
          stream_bytes_per_second * players * players,
  };
}

struct RelayPeer {
  std::uint64_t connection_id = 0;
  std::uint32_t role = 0;
  std::uint32_t session = 0;
};

enum class RelayDisposition : std::uint8_t {
  kForward,
  kUnknownConnection,
  kInvalidEnvelope,
  kSpoofedRole,
  kStaleSession,
  kInvalidTarget,
};

struct RelayRoute {
  RelayDisposition disposition = RelayDisposition::kInvalidEnvelope;
  std::vector<std::uint64_t> recipient_connections;
};

// Metadata-only visual relay. It authenticates sender identity and session,
// then forwards the original datagram bytes unchanged. It never needs retail
// skeletons, meshes, textures, or animation decoding.
class VisualRelayRouter {
 public:
  [[nodiscard]] bool Register(RelayPeer peer) {
    if (peer.connection_id == 0 || peer.role < 1 || peer.role > 100 ||
        peer.session == 0) {
      return false;
    }
    const auto role = role_to_connection_.find(peer.role);
    if (role != role_to_connection_.end() &&
        role->second != peer.connection_id) {
      peers_.erase(role->second);
    }
    const auto connection = peers_.find(peer.connection_id);
    if (connection != peers_.end() &&
        connection->second.role != peer.role) {
      role_to_connection_.erase(connection->second.role);
    }
    peers_[peer.connection_id] = peer;
    role_to_connection_[peer.role] = peer.connection_id;
    return true;
  }

  void Remove(std::uint64_t connection_id) {
    const auto peer = peers_.find(connection_id);
    if (peer == peers_.end()) {
      return;
    }
    role_to_connection_.erase(peer->second.role);
    peers_.erase(peer);
  }

  [[nodiscard]] RelayRoute Route(
      std::uint64_t source_connection,
      std::span<const std::uint8_t> datagram,
      std::uint32_t target_role = 0) const {
    const auto source = peers_.find(source_connection);
    if (source == peers_.end()) {
      return {
          .disposition = RelayDisposition::kUnknownConnection,
          .recipient_connections = {},
      };
    }
    protocol_v12::Envelope envelope;
    if (!protocol_v12::DecodeEnvelope(datagram, envelope)) {
      return {
          .disposition = RelayDisposition::kInvalidEnvelope,
          .recipient_connections = {},
      };
    }
    if (envelope.sender_role != source->second.role) {
      return {
          .disposition = RelayDisposition::kSpoofedRole,
          .recipient_connections = {},
      };
    }
    if (envelope.sender_session != source->second.session) {
      return {
          .disposition = RelayDisposition::kStaleSession,
          .recipient_connections = {},
      };
    }

    RelayRoute result{
        .disposition = RelayDisposition::kForward,
        .recipient_connections = {},
    };
    if (target_role != 0) {
      const auto target = role_to_connection_.find(target_role);
      if (target == role_to_connection_.end() ||
          target->second == source_connection) {
        result.disposition = RelayDisposition::kInvalidTarget;
        return result;
      }
      result.recipient_connections.push_back(target->second);
      return result;
    }
    result.recipient_connections.reserve(peers_.size() - 1);
    for (const auto& [connection_id, peer] : peers_) {
      (void)peer;
      if (connection_id != source_connection) {
        result.recipient_connections.push_back(connection_id);
      }
    }
    std::sort(result.recipient_connections.begin(),
              result.recipient_connections.end());
    return result;
  }

  [[nodiscard]] std::size_t peer_count() const { return peers_.size(); }

 private:
  std::unordered_map<std::uint64_t, RelayPeer> peers_;
  std::unordered_map<std::uint32_t, std::uint64_t> role_to_connection_;
};

}  // namespace skate3::multiplayer::routing
