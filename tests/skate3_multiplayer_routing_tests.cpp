#include "skate3_multiplayer_routing.h"
#include "skate3_multiplayer_transport.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace {

using namespace skate3::multiplayer;
using namespace skate3::multiplayer::protocol_v12;
using namespace skate3::multiplayer::routing;

int g_failures = 0;

void Expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++g_failures;
  }
}

std::vector<std::uint8_t> Datagram(std::uint16_t role,
                                   std::uint32_t session,
                                   std::uint32_t sequence = 1) {
  Envelope envelope{
      .kind = MessageKind::kRootSnapshot,
      .flags = kFlagExpires,
      .payload_bytes = 0,
      .sender_role = role,
      .stream_id = 1,
      .sender_session = session,
      .sequence = sequence,
      .sender_time_us = 1000,
  };
  std::vector<std::uint8_t> bytes(kEnvelopeBytes);
  Expect(EncodeEnvelope(envelope, bytes), "test envelope did not encode");
  return bytes;
}

void TestTopologyPolicyAndBudgets() {
  for (const std::uint32_t players : {2u, 5u, 20u}) {
    const auto route = RecommendRoute(players, true);
    Expect(route.supported && route.mode == RouteMode::kDirectPeerMesh &&
               !route.dedicated_preferred,
           "small/medium session did not retain direct P2P");
  }
  for (const std::uint32_t players : {50u, 100u}) {
    const auto with_server = RecommendRoute(players, true);
    const auto p2p_fallback = RecommendRoute(players, false);
    Expect(with_server.supported &&
               with_server.mode == RouteMode::kDedicatedRelay &&
               with_server.dedicated_preferred,
           "large session did not prefer dedicated relay");
    Expect(p2p_fallback.supported &&
               p2p_fallback.mode == RouteMode::kDirectPeerMesh &&
               p2p_fallback.dedicated_preferred,
           "large session lost explicit P2P fallback");
  }
  Expect(!RecommendRoute(1, true).supported &&
             !RecommendRoute(101, true).supported,
         "invalid player count was accepted");

  constexpr double stream = 114.0 * 1024.0;
  for (const std::uint32_t players : {2u, 5u, 20u, 50u, 100u}) {
    const auto mesh =
        ProjectBandwidth(players, stream, RouteMode::kDirectPeerMesh);
    const auto relay =
        ProjectBandwidth(players, stream, RouteMode::kDedicatedRelay);
    const double peers = static_cast<double>(players - 1);
    Expect(mesh.client_upload_bytes_per_second == stream * peers &&
               mesh.client_download_bytes_per_second == stream * peers,
           "mesh per-client projection changed");
    Expect(relay.client_upload_bytes_per_second == stream &&
               relay.client_download_bytes_per_second == stream * peers,
           "relay client projection changed");
    Expect(relay.relay_egress_bytes_per_second ==
               stream * static_cast<double>(players) * peers,
           "relay egress projection changed");
  }
}

void TestAuthenticatedVisualRelay() {
  VisualRelayRouter relay;
  for (std::uint32_t role = 1; role <= 100; ++role) {
    Expect(relay.Register({
               .connection_id = 1000 + role,
               .role = role,
               .session = 5000 + role,
           }),
           "valid relay peer did not register");
  }
  Expect(relay.peer_count() == 100, "relay peer count was not bounded to 100");

  const auto source = Datagram(50, 5050);
  const auto broadcast = relay.Route(1050, source);
  Expect(broadcast.disposition == RelayDisposition::kForward &&
             broadcast.recipient_connections.size() == 99,
         "100-player full-fidelity broadcast did not reach 99 peers");
  const auto directed = relay.Route(1050, source, 72);
  Expect(directed.disposition == RelayDisposition::kForward &&
             directed.recipient_connections ==
                 std::vector<std::uint64_t>{1072},
         "directed relay control did not reach exact role");
  Expect(relay.Route(9999, source).disposition ==
             RelayDisposition::kUnknownConnection,
         "unknown relay connection was accepted");
  Expect(relay.Route(1051, source).disposition ==
             RelayDisposition::kSpoofedRole,
         "relay accepted role spoofing");

  const auto stale = Datagram(50, 5051);
  Expect(relay.Route(1050, stale).disposition ==
             RelayDisposition::kStaleSession,
         "relay accepted stale sender generation");
  Expect(relay.Route(1050, source, 101).disposition ==
             RelayDisposition::kInvalidTarget,
         "relay accepted invalid target");

  relay.Remove(1072);
  Expect(relay.peer_count() == 99 &&
             relay.Route(1050, source, 72).disposition ==
                 RelayDisposition::kInvalidTarget,
         "relay retained removed connection");
  Expect(relay.Register({
             .connection_id = 2072,
             .role = 72,
             .session = 6072,
         }),
         "replacement relay generation did not register");
  Expect(relay.Route(1050, source, 72).recipient_connections ==
             std::vector<std::uint64_t>{2072},
         "role reuse did not route to new authenticated generation");
}

class RecordingTransport final : public TransportAdapter {
 public:
  [[nodiscard]] TransportKind kind() const override {
    return TransportKind::kDedicatedServer;
  }

  TransportBatchResult SendBatch(
      std::span<const TransportDatagramView> datagrams,
      std::uint64_t now_us) override {
    TransportBatchResult result;
    for (const auto& datagram : datagrams) {
      if (!datagram.Valid(now_us)) {
        ++result.rejected_datagrams;
        result.rejected_bytes += datagram.bytes.size();
        continue;
      }
      ++result.accepted_datagrams;
      result.accepted_bytes += datagram.bytes.size();
      ++snapshot_.sent_datagrams;
      snapshot_.sent_bytes += datagram.bytes.size();
    }
    snapshot_.captured_at_us = now_us;
    return result;
  }

  std::vector<TransportReceivedDatagram> ReceiveBatch(
      std::size_t maximum_datagrams, std::uint64_t now_us) override {
    (void)maximum_datagrams;
    (void)now_us;
    return {};
  }

  [[nodiscard]] TransportQueueSnapshot QueueSnapshot(
      std::uint64_t now_us) const override {
    TransportQueueSnapshot result = snapshot_;
    result.captured_at_us = now_us;
    return result;
  }

 private:
  TransportQueueSnapshot snapshot_;
};

void TestTransportBatchContract() {
  RecordingTransport transport;
  const auto bytes = Datagram(1, 100);
  const TransportEndpoint endpoint{
      .kind = TransportKind::kDedicatedServer,
      .connection_id = 7,
      .role = 2,
      .generation = 1,
  };
  const std::array<TransportDatagramView, 3> batch = {{
      {
          .target = endpoint,
          .traffic_class = OutboundTrafficClass::kControl,
          .expires_at_us = 0,
          .bytes = bytes,
      },
      {
          .target = endpoint,
          .traffic_class = OutboundTrafficClass::kRealtime,
          .expires_at_us = 2000,
          .bytes = bytes,
      },
      {
          .target = endpoint,
          .traffic_class = OutboundTrafficClass::kRealtime,
          .expires_at_us = 999,
          .bytes = bytes,
      },
  }};
  const auto sent = transport.SendBatch(batch, 1000);
  Expect(sent.accepted_datagrams == 2 && sent.rejected_datagrams == 1 &&
             sent.accepted_bytes == bytes.size() * 2,
         "transport batch did not preserve class/expiry contract");
  const auto queue = transport.QueueSnapshot(1500);
  Expect(queue.sent_datagrams == 2 &&
             queue.sent_bytes == bytes.size() * 2 &&
             queue.captured_at_us == 1500,
         "transport queue snapshot lost batch accounting");
}

}  // namespace

int main() {
  TestTopologyPolicyAndBudgets();
  TestAuthenticatedVisualRelay();
  TestTransportBatchContract();
  if (g_failures != 0) {
    std::cerr << g_failures << " routing/transport test(s) failed\n";
    return 1;
  }
  std::cout << "multiplayer routing/transport tests passed\n";
  return 0;
}
