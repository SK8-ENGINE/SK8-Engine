#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace skate3::multiplayer::simulation {

struct ImpairmentConfig {
  std::uint32_t rtt_ms = 0;
  std::uint32_t jitter_ms = 0;
  double random_loss_percent = 0.0;
  double reorder_percent = 0.0;
  double duplicate_percent = 0.0;
  std::uint64_t uplink_bits_per_second = 0;
  std::uint64_t burst_loss_start_us = 0;
  std::uint64_t burst_loss_duration_us = 0;
};

[[nodiscard]] inline bool ConfigValid(const ImpairmentConfig& config) {
  return config.rtt_ms <= 10'000 && config.jitter_ms <= 10'000 &&
         std::isfinite(config.random_loss_percent) &&
         config.random_loss_percent >= 0.0 &&
         config.random_loss_percent <= 100.0 &&
         std::isfinite(config.reorder_percent) &&
         config.reorder_percent >= 0.0 &&
         config.reorder_percent <= 100.0 &&
         std::isfinite(config.duplicate_percent) &&
         config.duplicate_percent >= 0.0 &&
         config.duplicate_percent <= 100.0;
}

struct SimulatedDatagram {
  std::uint32_t source = 0;
  std::uint32_t target = 0;
  std::uint32_t sequence = 0;
  std::uint64_t sent_at_us = 0;
  std::uint64_t delivered_at_us = 0;
  std::vector<std::uint8_t> bytes;
};

struct SimulatorStats {
  std::uint64_t submitted_datagrams = 0;
  std::uint64_t submitted_bytes = 0;
  std::uint64_t delivered_datagrams = 0;
  std::uint64_t delivered_bytes = 0;
  std::uint64_t random_drops = 0;
  std::uint64_t burst_drops = 0;
  std::uint64_t duplicates = 0;
  std::uint64_t reordered = 0;
  std::uint64_t maximum_queued_datagrams = 0;
  std::uint64_t maximum_queue_delay_us = 0;
};

// Deterministic offline datagram simulator used by protocol and scale tests.
// It models one-way propagation, signed jitter, random/burst loss, explicit
// reorder delay, duplication, and an uplink serialization queue. It never
// launches a client or touches the platform network stack.
class DatagramNetworkSimulator {
 public:
  explicit DatagramNetworkSimulator(ImpairmentConfig config = {},
                                    std::uint64_t seed = 1)
      : config_(config), random_state_(seed == 0 ? 1 : seed) {}

  [[nodiscard]] bool valid() const { return ConfigValid(config_); }

  bool Submit(std::uint32_t source, std::uint32_t target,
              std::uint32_t sequence, std::uint64_t now_us,
              std::span<const std::uint8_t> bytes) {
    if (!valid() || source == 0 || target == 0 || source == target ||
        bytes.empty()) {
      return false;
    }
    ++stats_.submitted_datagrams;
    stats_.submitted_bytes += bytes.size();
    if (InsideBurst(now_us)) {
      ++stats_.burst_drops;
      return true;
    }
    if (Chance(config_.random_loss_percent)) {
      ++stats_.random_drops;
      return true;
    }

    const std::uint64_t serialization_start =
        std::max(now_us, uplink_available_at_us_);
    std::uint64_t serialization_us = 0;
    if (config_.uplink_bits_per_second != 0) {
      const std::uint64_t bits =
          static_cast<std::uint64_t>(bytes.size()) * 8;
      serialization_us =
          (bits * 1'000'000 + config_.uplink_bits_per_second - 1) /
          config_.uplink_bits_per_second;
      uplink_available_at_us_ = serialization_start + serialization_us;
    } else {
      uplink_available_at_us_ = now_us;
    }

    std::uint64_t delivery =
        serialization_start + serialization_us +
        static_cast<std::uint64_t>(config_.rtt_ms) * 500;
    const std::int64_t jitter =
        SignedJitter(static_cast<std::uint64_t>(config_.jitter_ms) * 1000);
    if (jitter < 0 && static_cast<std::uint64_t>(-jitter) > delivery) {
      delivery = 0;
    } else {
      delivery = static_cast<std::uint64_t>(
          static_cast<std::int64_t>(delivery) + jitter);
    }
    if (Chance(config_.reorder_percent)) {
      delivery += static_cast<std::uint64_t>(config_.jitter_ms + 1) * 2000;
      ++stats_.reordered;
    }
    Queue(source, target, sequence, now_us, delivery, bytes);
    if (Chance(config_.duplicate_percent)) {
      Queue(source, target, sequence, now_us, delivery + 1000, bytes);
      ++stats_.duplicates;
    }
    stats_.maximum_queued_datagrams =
        std::max<std::uint64_t>(stats_.maximum_queued_datagrams,
                                queued_.size());
    stats_.maximum_queue_delay_us = std::max(
        stats_.maximum_queue_delay_us,
        delivery >= now_us ? delivery - now_us : 0);
    return true;
  }

  [[nodiscard]] std::vector<SimulatedDatagram> Poll(
      std::uint64_t now_us,
      std::size_t maximum_datagrams =
          std::numeric_limits<std::size_t>::max()) {
    std::stable_sort(
        queued_.begin(), queued_.end(),
        [](const Queued& left, const Queued& right) {
          if (left.datagram.delivered_at_us !=
              right.datagram.delivered_at_us) {
            return left.datagram.delivered_at_us <
                   right.datagram.delivered_at_us;
          }
          return left.order < right.order;
        });
    std::vector<SimulatedDatagram> result;
    result.reserve(std::min(maximum_datagrams, queued_.size()));
    std::size_t consumed = 0;
    while (consumed < queued_.size() &&
           result.size() < maximum_datagrams &&
           queued_[consumed].datagram.delivered_at_us <= now_us) {
      stats_.delivered_bytes += queued_[consumed].datagram.bytes.size();
      ++stats_.delivered_datagrams;
      result.push_back(std::move(queued_[consumed].datagram));
      ++consumed;
    }
    queued_.erase(queued_.begin(),
                  queued_.begin() + static_cast<std::ptrdiff_t>(consumed));
    return result;
  }

  [[nodiscard]] const SimulatorStats& stats() const { return stats_; }
  [[nodiscard]] std::size_t queued_datagrams() const {
    return queued_.size();
  }
  [[nodiscard]] std::uint64_t uplink_available_at_us() const {
    return uplink_available_at_us_;
  }

 private:
  struct Queued {
    std::uint64_t order = 0;
    SimulatedDatagram datagram;
  };

  [[nodiscard]] bool InsideBurst(std::uint64_t now_us) const {
    return config_.burst_loss_duration_us != 0 &&
           now_us >= config_.burst_loss_start_us &&
           now_us - config_.burst_loss_start_us <
               config_.burst_loss_duration_us;
  }

  [[nodiscard]] std::uint64_t Random() {
    random_state_ ^= random_state_ >> 12;
    random_state_ ^= random_state_ << 25;
    random_state_ ^= random_state_ >> 27;
    return random_state_ * 2685821657736338717ull;
  }

  [[nodiscard]] bool Chance(double percent) {
    if (percent <= 0.0) {
      return false;
    }
    if (percent >= 100.0) {
      return true;
    }
    const long double unit =
        static_cast<long double>(Random()) /
        static_cast<long double>(
            std::numeric_limits<std::uint64_t>::max());
    return unit < static_cast<long double>(percent / 100.0);
  }

  [[nodiscard]] std::int64_t SignedJitter(std::uint64_t maximum_us) {
    if (maximum_us == 0) {
      return 0;
    }
    const std::uint64_t range = maximum_us * 2 + 1;
    return static_cast<std::int64_t>(Random() % range) -
           static_cast<std::int64_t>(maximum_us);
  }

  void Queue(std::uint32_t source, std::uint32_t target,
             std::uint32_t sequence, std::uint64_t sent_at_us,
             std::uint64_t delivered_at_us,
             std::span<const std::uint8_t> bytes) {
    Queued queued;
    queued.order = next_order_++;
    queued.datagram.source = source;
    queued.datagram.target = target;
    queued.datagram.sequence = sequence;
    queued.datagram.sent_at_us = sent_at_us;
    queued.datagram.delivered_at_us = delivered_at_us;
    queued.datagram.bytes.assign(bytes.begin(), bytes.end());
    queued_.push_back(std::move(queued));
  }

  ImpairmentConfig config_;
  std::uint64_t random_state_ = 1;
  std::uint64_t uplink_available_at_us_ = 0;
  std::uint64_t next_order_ = 0;
  SimulatorStats stats_;
  std::vector<Queued> queued_;
};

}  // namespace skate3::multiplayer::simulation
