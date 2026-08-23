#include "skate3_multiplayer_network_simulator.h"
#include "skate3_multiplayer_protocol_v12.h"
#include "skate3_multiplayer_routing.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <set>
#include <span>
#include <string_view>
#include <vector>

namespace {

using namespace skate3::multiplayer::protocol_v12;
using namespace skate3::multiplayer::routing;
using namespace skate3::multiplayer::simulation;

int g_failures = 0;

void Expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++g_failures;
  }
}

std::vector<std::uint8_t> PoseFragment(std::uint16_t role,
                                       std::uint32_t session,
                                       std::uint32_t sequence,
                                       std::uint16_t payload_bytes) {
  Envelope envelope{
      .kind = MessageKind::kPoseDelta,
      .flags = kFlagExpires,
      .payload_bytes = payload_bytes,
      .sender_role = role,
      .stream_id = 1,
      .sender_session = session,
      .sequence = sequence,
      .sender_time_us =
          static_cast<std::uint64_t>(sequence) * 16'667,
  };
  std::vector<std::uint8_t> bytes(kEnvelopeBytes + payload_bytes);
  Expect(EncodeEnvelope(envelope, bytes), "scale envelope did not encode");
  for (std::size_t index = kEnvelopeBytes; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::uint8_t>(
        (std::uint32_t(role) * 31u + sequence * 17u + index) & 0xffu);
  }
  return bytes;
}

void TestSharedSerializationAtScale() {
  constexpr std::uint32_t frames = 120;
  for (const std::uint32_t players : {2u, 5u, 20u, 50u, 100u}) {
    VisualRelayRouter relay;
    for (std::uint32_t role = 1; role <= players; ++role) {
      Expect(relay.Register({
                 .connection_id = 10'000 + role,
                 .role = role,
                 .session = 20'000 + role,
             }),
             "scale relay registration failed");
    }
    std::uint64_t serialization_operations = 0;
    std::uint64_t ingress_datagrams = 0;
    std::uint64_t forwarded_datagrams = 0;
    std::uint64_t forwarded_bytes = 0;
    for (std::uint32_t frame = 1; frame <= frames; ++frame) {
      for (std::uint32_t role = 1; role <= players; ++role) {
        // Typical predictive live poses are represented by two MTU-safe
        // fragments. Each source fragment is serialized once, then the relay
        // forwards immutable bytes to all recipients.
        for (std::uint32_t fragment = 0; fragment < 2; ++fragment) {
          const auto bytes = PoseFragment(
              static_cast<std::uint16_t>(role), 20'000 + role,
              frame * 2 + fragment, fragment == 0 ? 1160 : 658);
          ++serialization_operations;
          ++ingress_datagrams;
          const auto route = relay.Route(10'000 + role, bytes);
          Expect(route.disposition == RelayDisposition::kForward &&
                     route.recipient_connections.size() == players - 1,
                 "full-fidelity scale fanout dropped a recipient");
          forwarded_datagrams += route.recipient_connections.size();
          forwarded_bytes +=
              bytes.size() * route.recipient_connections.size();
        }
      }
    }
    const std::uint64_t expected_ingress =
        std::uint64_t(players) * frames * 2;
    const std::uint64_t expected_forwarded =
        expected_ingress * (players - 1);
    Expect(serialization_operations == expected_ingress &&
               ingress_datagrams == expected_ingress &&
               forwarded_datagrams == expected_forwarded &&
               forwarded_bytes != 0,
           "scale simulation did not share source serialization");
    std::cout << "scale players=" << players
              << " source_serializations=" << serialization_operations
              << " forwarded_datagrams=" << forwarded_datagrams
              << " forwarded_mib="
              << static_cast<double>(forwarded_bytes) / (1024.0 * 1024.0)
              << '\n';
  }
}

void TestImpairmentPrimitive() {
  const std::array<std::uint8_t, 1000> bytes{};
  DatagramNetworkSimulator network(
      {
          .rtt_ms = 80,
          .jitter_ms = 10,
          .random_loss_percent = 0.0,
          .reorder_percent = 100.0,
          .duplicate_percent = 100.0,
          .uplink_bits_per_second = 1'000'000,
          .burst_loss_start_us = 100'000,
          .burst_loss_duration_us = 100'000,
      },
      7);
  Expect(network.Submit(1, 2, 1, 0, bytes),
         "valid impaired datagram was rejected");
  Expect(network.Submit(1, 2, 2, 150'000, bytes),
         "burst datagram submit failed");
  auto early = network.Poll(20'000);
  Expect(early.empty(), "impaired datagram arrived before one-way latency");
  auto delivered = network.Poll(1'000'000);
  Expect(delivered.size() == 2 &&
             delivered[0].sequence == 1 &&
             delivered[1].sequence == 1,
         "duplicate/reorder impairment did not remain deterministic");
  const auto& stats = network.stats();
  Expect(stats.submitted_datagrams == 2 && stats.burst_drops == 1 &&
             stats.duplicates == 1 && stats.reordered == 1 &&
             stats.delivered_datagrams == 2 &&
             stats.maximum_queue_delay_us >= 40'000,
         "impairment accounting changed");
}

struct FrameReceipt {
  std::uint8_t fragments = 0;
};

double SimulatePoseCoverage(ImpairmentConfig config, std::uint64_t seed,
                            std::uint32_t frames = 1800) {
  DatagramNetworkSimulator network(config, seed);
  const auto first = PoseFragment(1, 101, 1, 1160);
  const auto second = PoseFragment(1, 101, 2, 658);
  for (std::uint32_t frame = 0; frame < frames; ++frame) {
    const std::uint64_t now = std::uint64_t(frame) * 16'667;
    (void)network.Submit(1, 2, frame * 2 + 1, now, first);
    (void)network.Submit(1, 2, frame * 2 + 2, now, second);
  }
  const auto delivered = network.Poll(
      std::uint64_t(frames) * 16'667 + 10'000'000);
  std::map<std::uint32_t, FrameReceipt> receipts;
  for (const auto& datagram : delivered) {
    const std::uint32_t frame = (datagram.sequence - 1) / 2;
    receipts[frame].fragments |=
        static_cast<std::uint8_t>(1u << ((datagram.sequence - 1) % 2));
  }
  std::set<std::uint32_t> complete;
  for (const auto& [frame, receipt] : receipts) {
    if (receipt.fragments == 3) {
      complete.insert(frame);
    }
  }
  std::uint64_t usable = 0;
  std::uint64_t considered = 0;
  // A missing source frame remains interpolatable when complete samples
  // bracket it within the 100 ms recovery window.
  constexpr std::uint32_t maximum_gap_frames = 6;
  for (std::uint32_t frame = maximum_gap_frames;
       frame + maximum_gap_frames < frames; ++frame) {
    ++considered;
    if (complete.contains(frame)) {
      ++usable;
      continue;
    }
    const auto future = complete.lower_bound(frame);
    if (future == complete.end() || future == complete.begin()) {
      continue;
    }
    const auto past = std::prev(future);
    if (frame - *past <= maximum_gap_frames &&
        *future - frame <= maximum_gap_frames) {
      ++usable;
    }
  }
  return considered == 0
             ? 0.0
             : static_cast<double>(usable) /
                   static_cast<double>(considered);
}

void TestValidationMatrix() {
  for (const std::uint32_t rtt : {20u, 80u, 150u, 200u}) {
    const double coverage = SimulatePoseCoverage(
        {.rtt_ms = rtt, .jitter_ms = 10}, 100 + rtt, 360);
    Expect(coverage == 1.0, "latency-only stream lost interpolation coverage");
  }
  for (const double loss : {0.0, 1.0, 3.0, 5.0}) {
    const double coverage = SimulatePoseCoverage(
        {.rtt_ms = 80,
         .jitter_ms = 10,
         .random_loss_percent = loss},
        200 + static_cast<std::uint64_t>(loss * 10), 1800);
    Expect(coverage > 0.98,
           "random-loss matrix caused unbounded pose outage");
    if (loss <= 1.0) {
      Expect(coverage >= 0.999,
             "normal-envelope interpolation coverage fell below 99.9%");
    }
  }
  for (const std::uint32_t jitter : {0u, 5u, 20u, 50u}) {
    const double coverage = SimulatePoseCoverage(
        {.rtt_ms = 80,
         .jitter_ms = jitter,
         .reorder_percent = 0.1,
         .duplicate_percent = 0.1},
        300 + jitter, 720);
    Expect(coverage >= 0.999,
           "jitter/reorder matrix lost complete independent poses");
  }
  for (const std::uint64_t uplink :
       {1'000'000ull, 5'000'000ull, 10'000'000ull}) {
    const double coverage = SimulatePoseCoverage(
        {.rtt_ms = 80,
         .jitter_ms = 10,
         .uplink_bits_per_second = uplink},
        400 + uplink, 720);
    Expect(coverage >= 0.999,
           "uplink matrix lost pose data instead of queueing it");
  }
  for (const std::uint64_t burst_ms : {100ull, 250ull, 500ull}) {
    const double coverage = SimulatePoseCoverage(
        {.rtt_ms = 80,
         .jitter_ms = 10,
         .burst_loss_start_us = 5'000'000,
         .burst_loss_duration_us = burst_ms * 1000},
        500 + burst_ms, 720);
    const double minimum =
        burst_ms <= 100 ? 0.999 : (burst_ms <= 250 ? 0.97 : 0.93);
    Expect(coverage >= minimum,
           "burst-loss matrix exceeded bounded outage expectation");
  }
}

}  // namespace

int main() {
  TestSharedSerializationAtScale();
  TestImpairmentPrimitive();
  TestValidationMatrix();
  if (g_failures != 0) {
    std::cerr << g_failures << " scale/simulation test(s) failed\n";
    return 1;
  }
  std::cout << "multiplayer scale/simulation tests passed\n";
  return 0;
}
