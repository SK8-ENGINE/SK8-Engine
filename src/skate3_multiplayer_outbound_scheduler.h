#pragma once

#include "skate3_multiplayer_protocol_v12.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <utility>
#include <vector>

namespace skate3::multiplayer {

enum class OutboundTrafficClass {
  kControl,
  kRealtime,
  kAppearance,
};

[[nodiscard]] constexpr bool OutboundTrafficReliable(
    OutboundTrafficClass traffic_class) {
  return traffic_class != OutboundTrafficClass::kRealtime;
}

struct OutboundSchedulerLimits {
  std::size_t maximum_datagram_bytes =
      protocol_v12::kMaximumDatagramBytes;
  std::size_t maximum_control_bytes = 64 * 1024;
  std::size_t maximum_realtime_bytes = 256 * 1024;
  std::size_t maximum_appearance_bytes = 1024 * 1024;
};

struct OutboundMessage {
  std::uint64_t message_id = 0;
  std::uint64_t target_id = 0;
  std::uint32_t stream_id = 0;
  std::uint32_t sequence = 0;
  OutboundTrafficClass traffic_class =
      OutboundTrafficClass::kRealtime;
  bool reliable = false;
  std::uint64_t expires_at_us = 0;
  std::vector<std::uint8_t> bytes;
};

struct OutboundEnqueueRequest {
  std::uint64_t message_id = 0;
  std::uint64_t target_id = 0;
  std::uint32_t stream_id = 0;
  std::uint32_t sequence = 0;
  OutboundTrafficClass traffic_class =
      OutboundTrafficClass::kRealtime;
  bool reliable = false;
  std::uint64_t expires_at_us = 0;
  std::span<const std::uint8_t> bytes;
};

enum class OutboundEnqueueDisposition {
  kQueued,
  kReplacedOlderRealtime,
  kDuplicateRealtime,
  kStaleRealtime,
  kExpired,
  kClassLimit,
  kInvalid,
};

struct OutboundEnqueueResult {
  OutboundEnqueueDisposition disposition =
      OutboundEnqueueDisposition::kInvalid;
  std::size_t dropped_messages = 0;
  std::size_t dropped_bytes = 0;
};

struct OutboundDrainResult {
  std::vector<OutboundMessage> messages;
  std::size_t bytes = 0;
  std::size_t appearance_bytes = 0;
  std::size_t expired_messages = 0;
  std::size_t expired_bytes = 0;
};

class OutboundScheduler {
 public:
  explicit OutboundScheduler(
      OutboundSchedulerLimits limits = {})
      : limits_(limits) {}

  [[nodiscard]] OutboundEnqueueResult Enqueue(
      const OutboundEnqueueRequest& request,
      std::uint64_t now_us) {
    OutboundEnqueueResult result;
    if (!RequestShapeValid(request)) {
      return result;
    }
    if (request.traffic_class ==
            OutboundTrafficClass::kRealtime &&
        request.expires_at_us <= now_us) {
      result.disposition =
          OutboundEnqueueDisposition::kExpired;
      return result;
    }
    const std::size_t class_limit =
        ClassLimit(request.traffic_class);
    if (request.bytes.size() > class_limit) {
      result.disposition =
          OutboundEnqueueDisposition::kClassLimit;
      return result;
    }

    if (request.traffic_class ==
        OutboundTrafficClass::kRealtime) {
      const ExpiryResult expiry = ExpireRealtime(now_us);
      result.dropped_messages = expiry.messages;
      result.dropped_bytes = expiry.bytes;
      for (std::size_t index = 0;
           index < realtime_.size(); ++index) {
        const OutboundMessage& queued = realtime_[index];
        if (queued.target_id != request.target_id ||
            queued.stream_id != request.stream_id) {
          continue;
        }
        if (queued.sequence == request.sequence) {
          result.disposition =
              queued.bytes.size() == request.bytes.size() &&
                      std::equal(
                          queued.bytes.begin(), queued.bytes.end(),
                          request.bytes.begin())
                  ? OutboundEnqueueDisposition::kDuplicateRealtime
                  : OutboundEnqueueDisposition::kStaleRealtime;
          return result;
        }
        if (!protocol_v12::SequenceNewer(
                request.sequence, queued.sequence)) {
          result.disposition =
              OutboundEnqueueDisposition::kStaleRealtime;
          return result;
        }
        result.disposition =
            OutboundEnqueueDisposition::kReplacedOlderRealtime;
        ++result.dropped_messages;
        result.dropped_bytes += queued.bytes.size();
        realtime_bytes_ -= queued.bytes.size();
        realtime_.erase(
            realtime_.begin() +
            static_cast<std::ptrdiff_t>(index));
        break;
      }
    }

    std::deque<OutboundMessage>& queue =
        Queue(request.traffic_class);
    std::size_t& queued_bytes =
        ByteCount(request.traffic_class);

    if (request.traffic_class ==
        OutboundTrafficClass::kRealtime) {
      while (!queue.empty() &&
             queued_bytes + request.bytes.size() >
                 class_limit) {
        result.dropped_bytes += queue.front().bytes.size();
        ++result.dropped_messages;
        queued_bytes -= queue.front().bytes.size();
        queue.pop_front();
      }
    }
    if (queued_bytes + request.bytes.size() > class_limit) {
      result.disposition =
          OutboundEnqueueDisposition::kClassLimit;
      return result;
    }

    OutboundMessage message;
    message.message_id = request.message_id;
    message.target_id = request.target_id;
    message.stream_id = request.stream_id;
    message.sequence = request.sequence;
    message.traffic_class = request.traffic_class;
    message.reliable = request.reliable;
    message.expires_at_us = request.expires_at_us;
    message.bytes.assign(
        request.bytes.begin(), request.bytes.end());
    queued_bytes += message.bytes.size();
    queue.push_back(std::move(message));
    if (result.disposition !=
        OutboundEnqueueDisposition::kReplacedOlderRealtime) {
      result.disposition =
          OutboundEnqueueDisposition::kQueued;
    }
    return result;
  }

  [[nodiscard]] OutboundDrainResult Drain(
      std::uint64_t now_us,
      std::size_t total_byte_budget,
      std::size_t appearance_byte_budget) {
    OutboundDrainResult result;
    const ExpiryResult expiry = ExpireRealtime(now_us);
    result.expired_messages = expiry.messages;
    result.expired_bytes = expiry.bytes;

    if (!DrainQueue(
        control_, control_bytes_, total_byte_budget,
        total_byte_budget, result)) {
      return result;
    }
    if (!DrainQueue(
        realtime_, realtime_bytes_, total_byte_budget,
        total_byte_budget, result)) {
      return result;
    }
    (void)DrainQueue(
        appearance_, appearance_bytes_, total_byte_budget,
        appearance_byte_budget, result);
    return result;
  }

  void Clear() {
    control_.clear();
    realtime_.clear();
    appearance_.clear();
    control_bytes_ = 0;
    realtime_bytes_ = 0;
    appearance_bytes_ = 0;
  }

  [[nodiscard]] std::size_t queued_messages(
      OutboundTrafficClass traffic_class) const {
    return ConstQueue(traffic_class).size();
  }

  [[nodiscard]] std::size_t queued_bytes(
      OutboundTrafficClass traffic_class) const {
    return ConstByteCount(traffic_class);
  }

 private:
  struct ExpiryResult {
    std::size_t messages = 0;
    std::size_t bytes = 0;
  };

  [[nodiscard]] bool RequestShapeValid(
      const OutboundEnqueueRequest& request) const {
    if (request.message_id == 0 || request.target_id == 0 ||
        request.bytes.empty() ||
        request.bytes.size() >
            limits_.maximum_datagram_bytes) {
      return false;
    }
    switch (request.traffic_class) {
      case OutboundTrafficClass::kControl:
      case OutboundTrafficClass::kAppearance:
        return request.reliable && request.expires_at_us == 0;
      case OutboundTrafficClass::kRealtime:
        return !request.reliable && request.expires_at_us != 0;
    }
    return false;
  }

  [[nodiscard]] ExpiryResult ExpireRealtime(
      std::uint64_t now_us) {
    ExpiryResult result;
    for (std::size_t index = realtime_.size();
         index > 0; --index) {
      if (realtime_[index - 1].expires_at_us > now_us) {
        continue;
      }
      result.bytes += realtime_[index - 1].bytes.size();
      ++result.messages;
      realtime_bytes_ -= realtime_[index - 1].bytes.size();
      realtime_.erase(
          realtime_.begin() +
          static_cast<std::ptrdiff_t>(index - 1));
    }
    return result;
  }

  [[nodiscard]] static bool DrainQueue(
      std::deque<OutboundMessage>& queue,
      std::size_t& queued_bytes,
      std::size_t total_byte_budget,
      std::size_t class_byte_budget,
      OutboundDrainResult& result) {
    while (!queue.empty()) {
      const std::size_t message_bytes =
          queue.front().bytes.size();
      if (result.bytes + message_bytes > total_byte_budget) {
        return false;
      }
      const bool appearance =
          queue.front().traffic_class ==
          OutboundTrafficClass::kAppearance;
      if (appearance &&
          result.appearance_bytes + message_bytes >
              class_byte_budget) {
        return false;
      }
      queued_bytes -= message_bytes;
      result.bytes += message_bytes;
      if (appearance) {
        result.appearance_bytes += message_bytes;
      }
      result.messages.push_back(std::move(queue.front()));
      queue.pop_front();
    }
    return true;
  }

  [[nodiscard]] std::deque<OutboundMessage>& Queue(
      OutboundTrafficClass traffic_class) {
    switch (traffic_class) {
      case OutboundTrafficClass::kControl:
        return control_;
      case OutboundTrafficClass::kRealtime:
        return realtime_;
      case OutboundTrafficClass::kAppearance:
        return appearance_;
    }
    return realtime_;
  }

  [[nodiscard]] const std::deque<OutboundMessage>& ConstQueue(
      OutboundTrafficClass traffic_class) const {
    switch (traffic_class) {
      case OutboundTrafficClass::kControl:
        return control_;
      case OutboundTrafficClass::kRealtime:
        return realtime_;
      case OutboundTrafficClass::kAppearance:
        return appearance_;
    }
    return realtime_;
  }

  [[nodiscard]] std::size_t& ByteCount(
      OutboundTrafficClass traffic_class) {
    switch (traffic_class) {
      case OutboundTrafficClass::kControl:
        return control_bytes_;
      case OutboundTrafficClass::kRealtime:
        return realtime_bytes_;
      case OutboundTrafficClass::kAppearance:
        return appearance_bytes_;
    }
    return realtime_bytes_;
  }

  [[nodiscard]] std::size_t ConstByteCount(
      OutboundTrafficClass traffic_class) const {
    switch (traffic_class) {
      case OutboundTrafficClass::kControl:
        return control_bytes_;
      case OutboundTrafficClass::kRealtime:
        return realtime_bytes_;
      case OutboundTrafficClass::kAppearance:
        return appearance_bytes_;
    }
    return realtime_bytes_;
  }

  [[nodiscard]] std::size_t ClassLimit(
      OutboundTrafficClass traffic_class) const {
    switch (traffic_class) {
      case OutboundTrafficClass::kControl:
        return limits_.maximum_control_bytes;
      case OutboundTrafficClass::kRealtime:
        return limits_.maximum_realtime_bytes;
      case OutboundTrafficClass::kAppearance:
        return limits_.maximum_appearance_bytes;
    }
    return 0;
  }

  OutboundSchedulerLimits limits_;
  std::deque<OutboundMessage> control_;
  std::deque<OutboundMessage> realtime_;
  std::deque<OutboundMessage> appearance_;
  std::size_t control_bytes_ = 0;
  std::size_t realtime_bytes_ = 0;
  std::size_t appearance_bytes_ = 0;
};

}  // namespace skate3::multiplayer
