#include "skate3_multiplayer_outbound_scheduler.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>

namespace {

using namespace skate3::multiplayer;

int g_failures = 0;

void Expect(bool condition, std::string_view message) {
  if (condition) {
    return;
  }
  std::cerr << "FAIL: " << message << '\n';
  ++g_failures;
}

OutboundEnqueueRequest Request(
    std::uint64_t id,
    OutboundTrafficClass traffic_class,
    std::span<const std::uint8_t> bytes) {
  OutboundEnqueueRequest request;
  request.message_id = id;
  request.target_id = 2;
  request.stream_id = 3;
  request.sequence = static_cast<std::uint32_t>(id);
  request.traffic_class = traffic_class;
  request.reliable =
      traffic_class != OutboundTrafficClass::kRealtime;
  request.expires_at_us =
      traffic_class == OutboundTrafficClass::kRealtime
          ? 2000
          : 0;
  request.bytes = bytes;
  return request;
}

void TestPriorityAndAppearanceBudget() {
  Expect(!OutboundTrafficReliable(
             OutboundTrafficClass::kRealtime) &&
             OutboundTrafficReliable(
                 OutboundTrafficClass::kControl) &&
             OutboundTrafficReliable(
                 OutboundTrafficClass::kAppearance),
         "transport reliability mapping changed");
  OutboundScheduler scheduler;
  const std::array<std::uint8_t, 400> appearance{};
  const std::array<std::uint8_t, 100> realtime{};
  const std::array<std::uint8_t, 40> control{};
  Expect(scheduler.Enqueue(
             Request(
                 1, OutboundTrafficClass::kAppearance,
                 appearance),
             1000)
             .disposition ==
             OutboundEnqueueDisposition::kQueued,
         "appearance was not queued");
  Expect(scheduler.Enqueue(
             Request(
                 2, OutboundTrafficClass::kRealtime,
                 realtime),
             1000)
             .disposition ==
             OutboundEnqueueDisposition::kQueued,
         "realtime was not queued");
  Expect(scheduler.Enqueue(
             Request(
                 3, OutboundTrafficClass::kControl,
                 control),
             1000)
             .disposition ==
             OutboundEnqueueDisposition::kQueued,
         "control was not queued");

  OutboundDrainResult drained =
      scheduler.Drain(1000, 1000, 300);
  Expect(drained.messages.size() == 2 &&
             drained.messages[0].traffic_class ==
                 OutboundTrafficClass::kControl &&
             drained.messages[1].traffic_class ==
                 OutboundTrafficClass::kRealtime,
         "scheduler did not prioritize control then realtime");
  Expect(drained.bytes == 140 &&
             drained.appearance_bytes == 0,
         "appearance exceeded its independent drain budget");
  Expect(scheduler.queued_messages(
             OutboundTrafficClass::kAppearance) == 1,
         "deferred appearance was discarded");

  drained = scheduler.Drain(1001, 1000, 400);
  Expect(drained.messages.size() == 1 &&
             drained.messages[0].traffic_class ==
                 OutboundTrafficClass::kAppearance &&
             drained.appearance_bytes == 400,
         "appearance did not resume under its own budget");
}

void TestRealtimeLatestWins() {
  OutboundScheduler scheduler;
  const std::array<std::uint8_t, 100> bytes{};
  OutboundEnqueueRequest request =
      Request(10, OutboundTrafficClass::kRealtime, bytes);
  request.sequence = UINT32_MAX;
  Expect(scheduler.Enqueue(request, 1000).disposition ==
             OutboundEnqueueDisposition::kQueued,
         "first realtime message was not queued");

  request.message_id = 11;
  request.sequence = 0;
  const OutboundEnqueueResult replacement =
      scheduler.Enqueue(request, 1000);
  Expect(replacement.disposition ==
             OutboundEnqueueDisposition::kReplacedOlderRealtime &&
             replacement.dropped_messages == 1 &&
             replacement.dropped_bytes == bytes.size(),
         "newer realtime message did not replace old at rollover");
  Expect(scheduler.queued_messages(
             OutboundTrafficClass::kRealtime) == 1,
         "realtime replacement changed queue count");

  Expect(scheduler.Enqueue(request, 1000).disposition ==
             OutboundEnqueueDisposition::kDuplicateRealtime,
         "identical realtime duplicate was not detected");
  request.message_id = 12;
  request.sequence = UINT32_MAX;
  Expect(scheduler.Enqueue(request, 1000).disposition ==
             OutboundEnqueueDisposition::kStaleRealtime,
         "obsolete realtime message was accepted");
  const OutboundDrainResult drained =
      scheduler.Drain(1000, 1200, 0);
  Expect(drained.messages.size() == 1 &&
             drained.messages[0].sequence == 0,
         "scheduler drained wrong realtime generation");
}

void TestRealtimeExpiryAndClassValidation() {
  OutboundScheduler scheduler;
  const std::array<std::uint8_t, 50> bytes{};
  OutboundEnqueueRequest realtime =
      Request(20, OutboundTrafficClass::kRealtime, bytes);
  realtime.expires_at_us = 1000;
  Expect(scheduler.Enqueue(realtime, 1000).disposition ==
             OutboundEnqueueDisposition::kExpired,
         "already expired realtime message was queued");
  realtime.expires_at_us = 1100;
  Expect(scheduler.Enqueue(realtime, 1000).disposition ==
             OutboundEnqueueDisposition::kQueued,
         "fresh realtime message was rejected");
  const OutboundDrainResult expired =
      scheduler.Drain(1100, 1200, 1200);
  Expect(expired.messages.empty() &&
             expired.expired_messages == 1 &&
             expired.expired_bytes == bytes.size(),
         "expired realtime message was transmitted");

  OutboundEnqueueRequest invalid =
      Request(21, OutboundTrafficClass::kRealtime, bytes);
  invalid.reliable = true;
  Expect(scheduler.Enqueue(invalid, 1000).disposition ==
             OutboundEnqueueDisposition::kInvalid,
         "reliable realtime message was accepted");
  invalid = Request(
      22, OutboundTrafficClass::kAppearance, bytes);
  invalid.expires_at_us = 2000;
  Expect(scheduler.Enqueue(invalid, 1000).disposition ==
             OutboundEnqueueDisposition::kInvalid,
         "expirable reliable appearance was accepted");
}

void TestAppearanceOverloadCannotConsumeRealtimeCapacity() {
  OutboundSchedulerLimits limits;
  limits.maximum_datagram_bytes = 1200;
  limits.maximum_control_bytes = 100;
  limits.maximum_realtime_bytes = 200;
  limits.maximum_appearance_bytes = 300;
  OutboundScheduler scheduler(limits);
  const std::array<std::uint8_t, 100> bytes{};

  for (std::uint64_t id = 1; id <= 3; ++id) {
    OutboundEnqueueRequest appearance =
        Request(
            id, OutboundTrafficClass::kAppearance, bytes);
    appearance.target_id = id;
    Expect(scheduler.Enqueue(appearance, 1000).disposition ==
               OutboundEnqueueDisposition::kQueued,
           "appearance did not fill isolated class budget");
  }
  OutboundEnqueueRequest excess =
      Request(4, OutboundTrafficClass::kAppearance, bytes);
  excess.target_id = 4;
  Expect(scheduler.Enqueue(excess, 1000).disposition ==
             OutboundEnqueueDisposition::kClassLimit,
         "appearance exceeded isolated class budget");

  OutboundEnqueueRequest realtime =
      Request(5, OutboundTrafficClass::kRealtime, bytes);
  realtime.target_id = 5;
  Expect(scheduler.Enqueue(realtime, 1000).disposition ==
             OutboundEnqueueDisposition::kQueued,
         "appearance overload blocked realtime enqueue");
  OutboundEnqueueRequest control =
      Request(6, OutboundTrafficClass::kControl, bytes);
  control.target_id = 6;
  Expect(scheduler.Enqueue(control, 1000).disposition ==
             OutboundEnqueueDisposition::kQueued,
         "appearance overload blocked control enqueue");

  const OutboundDrainResult drained =
      scheduler.Drain(1000, 200, 1000);
  Expect(drained.messages.size() == 2 &&
             drained.messages[0].traffic_class ==
                 OutboundTrafficClass::kControl &&
             drained.messages[1].traffic_class ==
                 OutboundTrafficClass::kRealtime,
         "appearance overload consumed priority drain budget");
}

void TestRealtimeQueueEvictsOldestWithinClass() {
  OutboundSchedulerLimits limits;
  limits.maximum_realtime_bytes = 200;
  OutboundScheduler scheduler(limits);
  const std::array<std::uint8_t, 100> bytes{};
  for (std::uint64_t id = 1; id <= 2; ++id) {
    OutboundEnqueueRequest request =
        Request(
            id, OutboundTrafficClass::kRealtime, bytes);
    request.target_id = id;
    Expect(scheduler.Enqueue(request, 1000).disposition ==
               OutboundEnqueueDisposition::kQueued,
           "realtime class did not fill");
  }
  OutboundEnqueueRequest newest =
      Request(3, OutboundTrafficClass::kRealtime, bytes);
  newest.target_id = 3;
  const OutboundEnqueueResult result =
      scheduler.Enqueue(newest, 1000);
  Expect(result.disposition ==
             OutboundEnqueueDisposition::kQueued &&
             result.dropped_messages == 1 &&
             result.dropped_bytes == 100,
         "realtime overload did not evict oldest message");
  const OutboundDrainResult drained =
      scheduler.Drain(1000, 1000, 0);
  Expect(drained.messages.size() == 2 &&
             drained.messages[0].target_id == 2 &&
             drained.messages[1].target_id == 3,
         "realtime class did not retain newest messages");
  scheduler.Clear();
  Expect(scheduler.queued_bytes(
             OutboundTrafficClass::kRealtime) == 0,
         "scheduler clear retained queue accounting");
}

void TestPriorityDoesNotBypassBlockedControl() {
  OutboundScheduler scheduler;
  const std::array<std::uint8_t, 100> control_bytes{};
  const std::array<std::uint8_t, 40> realtime_bytes{};
  Expect(scheduler.Enqueue(
             Request(
                 30, OutboundTrafficClass::kControl,
                 control_bytes),
             1000)
             .disposition ==
             OutboundEnqueueDisposition::kQueued,
         "blocked-priority control did not queue");
  OutboundEnqueueRequest realtime =
      Request(
          31, OutboundTrafficClass::kRealtime,
          realtime_bytes);
  realtime.target_id = 3;
  Expect(scheduler.Enqueue(realtime, 1000).disposition ==
             OutboundEnqueueDisposition::kQueued,
         "blocked-priority realtime did not queue");
  const OutboundDrainResult drained =
      scheduler.Drain(1000, 50, 50);
  Expect(drained.messages.empty(),
         "realtime bypassed control that did not fit budget");

  OutboundSchedulerLimits limits;
  limits.maximum_realtime_bytes = 100;
  OutboundScheduler limited(limits);
  const std::array<std::uint8_t, 50> old_bytes{};
  const std::array<std::uint8_t, 150> oversized{};
  OutboundEnqueueRequest old =
      Request(
          32, OutboundTrafficClass::kRealtime, old_bytes);
  Expect(limited.Enqueue(old, 1000).disposition ==
             OutboundEnqueueDisposition::kQueued,
         "class-limit test old realtime did not queue");
  OutboundEnqueueRequest newer =
      Request(
          33, OutboundTrafficClass::kRealtime, oversized);
  newer.sequence = old.sequence + 1;
  Expect(limited.Enqueue(newer, 1000).disposition ==
             OutboundEnqueueDisposition::kClassLimit,
         "oversized replacement exceeded realtime class");
  Expect(limited.queued_messages(
             OutboundTrafficClass::kRealtime) == 1 &&
             limited.queued_bytes(
                 OutboundTrafficClass::kRealtime) ==
                 old_bytes.size(),
         "rejected replacement discarded valid old realtime");
}

}  // namespace

int main() {
  TestPriorityAndAppearanceBudget();
  TestRealtimeLatestWins();
  TestRealtimeExpiryAndClassValidation();
  TestAppearanceOverloadCannotConsumeRealtimeCapacity();
  TestRealtimeQueueEvictsOldestWithinClass();
  TestPriorityDoesNotBypassBlockedControl();

  if (g_failures != 0) {
    std::cerr << g_failures
              << " multiplayer outbound scheduler test(s) failed\n";
    return 1;
  }
  std::cout
      << "All multiplayer outbound scheduler tests passed\n";
  return 0;
}
