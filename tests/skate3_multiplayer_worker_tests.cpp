#include "skate3_multiplayer_interpolation.h"
#include "skate3_multiplayer_worker.h"
#include "skate3_multiplayer_latest_request.h"
#include "skate3_multiplayer_motion_trace.h"
#include "skate3_multiplayer_playback_clock.h"
#include "skate3_multiplayer_pose_cadence.h"
#include "skate3_multiplayer_pose_curve.h"
#include "skate3_multiplayer_send_schedule.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

namespace {

struct TestInput {
  std::uint64_t id = 0;
};

struct TestPlayer {
  std::uint64_t id = 0;
};

struct TestRetirement {
  std::uint64_t id = 0;
};

using Mailbox =
    skate3::multiplayer::worker::LatestFrameMailbox<
        TestInput, TestPlayer, TestRetirement>;

struct TestRequestKey {
  std::uint32_t session = 0;
  std::uint64_t appearance = 0;

  bool operator==(const TestRequestKey&) const = default;
};

struct TestRequestResult {
  std::uint64_t appearance = 0;
};

using ResultTable =
    skate3::multiplayer::worker::LatestResultTable<
        std::uint32_t, TestRequestKey, TestRequestResult>;

int g_failures = 0;

void Expect(bool condition, std::string_view message) {
  if (condition) {
    return;
  }
  std::cerr << "FAIL: " << message << '\n';
  ++g_failures;
}

std::shared_ptr<const std::vector<TestPlayer>> Players(
    std::uint64_t id) {
  return std::make_shared<const std::vector<TestPlayer>>(
      std::vector<TestPlayer>{{id}});
}

void TestLatestInputWins() {
  Mailbox mailbox;

  Expect(!mailbox.PublishInput(
             std::make_shared<const TestInput>(TestInput{1})),
         "first input unexpectedly replaced a pending capture");
  Expect(mailbox.HasPendingInput(),
         "published input was not visible to the consumer");
  Expect(mailbox.PublishInput(
             std::make_shared<const TestInput>(TestInput{2})),
         "superseding input did not report a replacement");

  const std::shared_ptr<const TestInput> input = mailbox.TakeInput();
  Expect(input != nullptr && input->id == 2,
         "consumer did not receive the latest local capture");
  Expect(!mailbox.HasPendingInput(),
         "taking the latest input did not drain the slot");
  Expect(mailbox.TakeInput() == nullptr,
         "an empty input slot returned a duplicate capture");
}

void TestImmutablePresentationAndSequences() {
  Mailbox mailbox;
  const auto first_players = Players(10);

  Expect(mailbox.PublishPresentation(
             first_players, std::vector<TestRetirement>{{1}}) == 1,
         "first presentation sequence was not one");
  Expect(mailbox.PublishPresentation(
             Players(20), std::vector<TestRetirement>{{2}}) == 2,
         "presentation sequence did not advance monotonically");
  Expect(first_players->size() == 1 && first_players->front().id == 10,
         "publishing a replacement mutated an in-use presentation");

  std::uint64_t sequence = 0;
  std::shared_ptr<const std::vector<TestPlayer>> players;
  std::vector<TestRetirement> retirements;
  Expect(mailbox.ConsumePresentation(sequence, players, retirements),
         "latest non-empty presentation was not reported");
  Expect(sequence == 2 && players != nullptr &&
             players->size() == 1 && players->front().id == 20,
         "consumer did not receive the latest presentation");
  Expect(retirements.size() == 2 &&
             retirements[0].id == 1 && retirements[1].id == 2,
         "unconsumed retirement events did not accumulate in order");

  retirements.push_back({99});
  Expect(mailbox.ConsumePresentation(sequence, players, retirements),
         "latest player presentation was not retained between renders");
  Expect(sequence == 2 && players->front().id == 20,
         "stable presentation changed without a publication");
  Expect(retirements.empty(),
         "one-shot retirement events were delivered more than once");
}

void TestEmptyPresentationAndClear() {
  Mailbox mailbox;
  (void)mailbox.PublishInput(
      std::make_shared<const TestInput>(TestInput{7}));
  (void)mailbox.PublishPresentation(
      std::make_shared<const std::vector<TestPlayer>>(),
      std::vector<TestRetirement>{{8}});
  mailbox.Clear();

  std::uint64_t sequence = 99;
  std::shared_ptr<const std::vector<TestPlayer>> players;
  std::vector<TestRetirement> retirements{{99}};
  Expect(!mailbox.HasPendingInput() && mailbox.TakeInput() == nullptr,
         "clear did not remove a pending input");
  Expect(!mailbox.ConsumePresentation(sequence, players, retirements),
         "clear left a visible player presentation");
  Expect(sequence == 0 && players == nullptr && retirements.empty(),
         "clear did not reset output sequence and retirement state");
}

void TestConcurrentHandoff() {
  constexpr std::uint64_t kIterations = 10000;
  Mailbox mailbox;
  std::atomic<bool> input_done = false;
  std::atomic<bool> presentation_done = false;
  std::uint64_t latest_input = 0;
  std::uint64_t latest_sequence = 0;
  std::uint64_t latest_player = 0;
  std::uint64_t retirement_count = 0;
  std::uint64_t retirement_sum = 0;

  std::thread input_producer([&] {
    for (std::uint64_t id = 1; id <= kIterations; ++id) {
      (void)mailbox.PublishInput(
          std::make_shared<const TestInput>(TestInput{id}));
    }
    input_done.store(true, std::memory_order_release);
  });
  std::thread input_consumer([&] {
    while (!input_done.load(std::memory_order_acquire) ||
           mailbox.HasPendingInput()) {
      if (const auto input = mailbox.TakeInput(); input != nullptr) {
        latest_input = input->id;
      } else {
        std::this_thread::yield();
      }
    }
  });
  std::thread presentation_producer([&] {
    for (std::uint64_t id = 1; id <= kIterations; ++id) {
      (void)mailbox.PublishPresentation(
          Players(id), std::vector<TestRetirement>{{id}});
    }
    presentation_done.store(true, std::memory_order_release);
  });
  std::thread presentation_consumer([&] {
    do {
      std::shared_ptr<const std::vector<TestPlayer>> players;
      std::vector<TestRetirement> retirements;
      (void)mailbox.ConsumePresentation(
          latest_sequence, players, retirements);
      if (players != nullptr && !players->empty()) {
        latest_player = players->front().id;
      }
      for (const TestRetirement& retirement : retirements) {
        ++retirement_count;
        retirement_sum += retirement.id;
      }
      if (!presentation_done.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    } while (!presentation_done.load(std::memory_order_acquire));

    std::shared_ptr<const std::vector<TestPlayer>> players;
    std::vector<TestRetirement> retirements;
    (void)mailbox.ConsumePresentation(
        latest_sequence, players, retirements);
    if (players != nullptr && !players->empty()) {
      latest_player = players->front().id;
    }
    for (const TestRetirement& retirement : retirements) {
      ++retirement_count;
      retirement_sum += retirement.id;
    }
  });

  input_producer.join();
  input_consumer.join();
  presentation_producer.join();
  presentation_consumer.join();

  Expect(latest_input == kIterations,
         "concurrent input handoff did not finish on the newest capture");
  Expect(latest_sequence == kIterations,
         "concurrent presentation sequence lost a publication");
  Expect(latest_player == kIterations,
         "concurrent presentation did not finish on the newest players");
  Expect(retirement_count == kIterations,
         "concurrent handoff lost or duplicated retirement events");
  Expect(retirement_sum == kIterations * (kIterations + 1) / 2,
         "concurrent retirement payloads were corrupted");
}

void TestLatestRequestRejectsStalePublication() {
  using Status =
      skate3::multiplayer::worker::LatestResultStatus;
  ResultTable table;
  const TestRequestKey old_key{7, 100};
  const TestRequestKey new_key{7, 200};

  Expect(table.Begin(2, old_key),
         "first keyed request was not accepted");
  Expect(!table.Begin(2, old_key),
         "duplicate keyed request restarted pending work");
  Expect(table.Begin(2, new_key),
         "new appearance did not supersede old work");
  Expect(!table.Publish(
             2, old_key,
             std::make_shared<const TestRequestResult>(
                 TestRequestResult{100})),
         "stale appearance result was published");

  std::shared_ptr<const TestRequestResult> result;
  Expect(table.Poll(2, old_key, result) == Status::kUnknown,
         "superseded key remained externally visible");
  Expect(table.Poll(2, new_key, result) == Status::kPending,
         "newest appearance did not remain pending");
  Expect(table.Publish(
             2, new_key,
             std::make_shared<const TestRequestResult>(
                 TestRequestResult{200})),
         "current appearance result was rejected");
  Expect(table.Poll(2, new_key, result) == Status::kReady &&
             result != nullptr && result->appearance == 200,
         "current appearance result was not returned");
}

void TestLatestRequestFailureAndGenerationForget() {
  using Status =
      skate3::multiplayer::worker::LatestResultStatus;
  ResultTable table;
  const TestRequestKey first_session{11, 300};
  const TestRequestKey next_session{12, 300};

  Expect(table.Begin(3, first_session),
         "first session request was not accepted");
  Expect(table.Publish(3, first_session, nullptr),
         "current failed result was rejected");
  std::shared_ptr<const TestRequestResult> result;
  Expect(
      table.Poll(3, first_session, result) == Status::kFailed &&
          result == nullptr,
      "failed result did not remain terminal for its key");
  Expect(
      !table.ForgetIf(
          3, [](const TestRequestKey& key) {
            return key.session == 99;
          }),
      "mismatched retirement erased current generation");
  Expect(
      table.Poll(3, first_session, result) == Status::kFailed,
      "mismatched retirement changed current generation");
  Expect(
      table.ForgetIf(
          3, [](const TestRequestKey& key) {
            return key.session == 11;
          }),
      "matching retirement did not erase current generation");
  Expect(
      table.Poll(3, first_session, result) == Status::kUnknown,
      "retired generation remained visible");
  Expect(table.Begin(3, next_session),
         "reconnected generation was not accepted");
}

void TestLatestRequestTransfersResultOwnership() {
  using Status =
      skate3::multiplayer::worker::LatestResultStatus;
  ResultTable table;
  const TestRequestKey old_key{20, 400};
  const TestRequestKey new_key{20, 500};
  Expect(table.Begin(4, old_key),
         "ownership test request was not accepted");
  Expect(table.Publish(
             4, old_key,
             std::make_shared<const TestRequestResult>(
                 TestRequestResult{400})),
         "ownership test result was not published");

  std::shared_ptr<const TestRequestResult> displaced;
  Expect(table.Begin(4, new_key, &displaced) &&
             displaced != nullptr &&
             displaced->appearance == 400,
         "superseded result ownership was not transferred");
  std::shared_ptr<const TestRequestResult> result;
  Expect(table.Poll(4, new_key, result) == Status::kPending,
         "replacement request was not pending after displacement");
  Expect(table.Publish(
             4, new_key,
             std::make_shared<const TestRequestResult>(
                 TestRequestResult{500})),
         "replacement result was not published");
  const auto taken = table.TakeIf(
      4, [](const TestRequestKey& key) {
        return key.session == 20;
      });
  Expect(taken != nullptr && taken->appearance == 500,
         "matching result ownership was not extracted");
  Expect(table.Poll(4, new_key, result) == Status::kUnknown,
         "extracted result remained visible");
}

void TestPeriodicDeadlineRetainsTargetRate() {
  using Deadline =
      skate3::multiplayer::schedule::PeriodicDeadline;
  using namespace std::chrono_literals;

  Deadline deadline;
  const auto start = Deadline::TimePoint{};
  std::uint32_t sends = 0;
  for (auto elapsed = 0ms; elapsed <= 1000ms; elapsed += 4ms) {
    const auto now = start + elapsed;
    if (deadline.Due(now)) {
      ++sends;
      deadline.Commit(now, 16666us);
    }
  }
  Expect(sends >= 60 && sends <= 61,
         "4 ms worker ticks collapsed a 60 Hz deadline to 50 Hz");
}

void TestPeriodicDeadlineDoesNotBurstAfterStall() {
  using Deadline =
      skate3::multiplayer::schedule::PeriodicDeadline;
  using namespace std::chrono_literals;

  Deadline deadline;
  const auto start = Deadline::TimePoint{};
  Expect(deadline.Due(start),
         "fresh periodic deadline was not immediately due");
  deadline.Commit(start, 16666us);

  const auto stalled = start + 500ms;
  Expect(deadline.Due(stalled),
         "stalled periodic deadline did not become due");
  deadline.Commit(stalled, 16666us);
  Expect(!deadline.Due(stalled),
         "stalled periodic deadline requested a catch-up burst");
  Expect(deadline.next() > stalled &&
             deadline.next() <= stalled + 16666us,
         "stalled periodic deadline did not advance to the next phase");

  deadline.Reset();
  Expect(deadline.Due(stalled),
         "reset periodic deadline was not immediately due");
}

void TestAdaptiveInterpolationDelayCoversMeasuredStalls() {
  using skate3::multiplayer::interpolation::
      RecommendedDelayMicroseconds;

  Expect(
      RecommendedDelayMicroseconds(
          50, 20000, 8000, true) == 164000,
      "measured five-client jitter did not receive enough safety delay");
  Expect(
      RecommendedDelayMicroseconds(
          50, 16300, 3300, true) == 107900,
      "smooth sender was assigned the wrong adaptive delay");
  Expect(
      RecommendedDelayMicroseconds(
          120, 16300, 3300, true) == 120000,
      "configured interpolation floor was not preserved");
  Expect(
      RecommendedDelayMicroseconds(
          50, 50000, 50000, true) == 250000,
      "adaptive interpolation delay exceeded its safety cap");
  Expect(
      RecommendedDelayMicroseconds(
          50, 50000, 50000, false) == 50000,
      "empty animation history ignored the configured delay");
}

void TestPresentationClockRejectsCursorJumps() {
  using skate3::multiplayer::playback::PresentationClock;

  PresentationClock clock;
  Expect(clock.Advance(1000000, 900000) == 900000,
         "presentation clock did not initialize at the ideal cursor");

  const std::int64_t normal =
      clock.Advance(1004000, 904000);
  Expect(normal == 904000,
         "presentation clock changed a stable one-to-one cadence");

  const std::int64_t delayed =
      clock.Advance(1008000, 858000);
  Expect(delayed == 907600,
         "presentation clock did not bound a backwards delay jump");
  Expect(delayed >= normal,
         "presentation clock moved backwards");
  Expect(clock.applied_correction_us() == -400,
         "presentation clock exceeded its negative slew limit");

  const std::int64_t advanced =
      clock.Advance(1012000, 962000);
  Expect(advanced == 912000,
         "presentation clock did not bound a forwards delay jump");
  Expect(clock.applied_correction_us() == 400,
         "presentation clock exceeded its positive slew limit");
  Expect(clock.ideal_error_us() == 50000,
         "presentation clock reported the wrong ideal-cursor error");
}

void TestPresentationClockConvergesWithoutRewinding() {
  using skate3::multiplayer::playback::PresentationClock;

  PresentationClock clock;
  std::int64_t local_us = 1000000;
  std::int64_t ideal_us = 900000;
  std::int64_t previous_us =
      clock.Advance(local_us, ideal_us);

  // Increase desired buffering by 40 ms. The cursor should run at 90% until
  // it reaches the new delay, never stopping or moving backwards.
  for (int tick = 0; tick < 120; ++tick) {
    local_us += 4000;
    ideal_us += 4000;
    const std::int64_t desired_with_more_delay =
        ideal_us - 40000;
    const std::int64_t current_us =
        clock.Advance(local_us, desired_with_more_delay);
    Expect(current_us >= previous_us,
           "presentation clock rewound while increasing its delay");
    Expect(current_us - previous_us >= 3600 &&
               current_us - previous_us <= 4000,
           "presentation clock exceeded its bounded slow cadence");
    previous_us = current_us;
  }
  Expect(clock.ideal_error_us() == 0,
         "presentation clock did not converge on increased delay");

  // Decrease desired buffering by 40 ms. Convergence may run at 110%, but
  // must still be gradual rather than a visible one-frame jump.
  for (int tick = 0; tick < 120; ++tick) {
    local_us += 4000;
    ideal_us += 4000;
    const std::int64_t current_us =
        clock.Advance(local_us, ideal_us);
    Expect(current_us - previous_us >= 4000 &&
               current_us - previous_us <= 4400,
           "presentation clock exceeded its bounded fast cadence");
    previous_us = current_us;
  }
  Expect(clock.ideal_error_us() == 0,
         "presentation clock did not converge on reduced delay");
}

void TestPresentationClockSmoothsSteppedSceneAnchors() {
  using skate3::multiplayer::playback::PresentationClock;

  PresentationClock clock;
  std::int64_t wall_time_us = 1000000;
  std::int64_t scene_anchor_us = 900000;
  std::int64_t previous_us =
      clock.Advance(wall_time_us, scene_anchor_us);

  // The native scene timestamp may be published at a lower cadence than the
  // replication worker. Repeated anchors followed by one larger step must
  // not stop and then jump every remote presentation cursor.
  for (int tick = 1; tick <= 120; ++tick) {
    wall_time_us += 4000;
    if (tick % 4 == 0) {
      scene_anchor_us += 16000;
    }
    const std::int64_t current_us =
        clock.Advance(wall_time_us, scene_anchor_us);
    Expect(current_us - previous_us >= 3600 &&
               current_us - previous_us <= 4400,
           "stepped scene anchors leaked into presentation cadence");
    previous_us = current_us;
  }
}

void TestBoundedPoseCurvePreservesSamplesAndLimits() {
  using skate3::multiplayer::pose_curve::
      InterpolateBoundedHermite;

  constexpr std::uint64_t kStep = 16667;
  Expect(
      InterpolateBoundedHermite(
          0.0f, 1.0f, 2.0f, 3.0f,
          0, kStep, kStep * 2, kStep * 3, 0.0f) == 1.0f,
      "pose curve did not preserve its first sample");
  Expect(
      InterpolateBoundedHermite(
          0.0f, 1.0f, 2.0f, 3.0f,
          0, kStep, kStep * 2, kStep * 3, 1.0f) == 2.0f,
      "pose curve did not preserve its second sample");

  for (int step = 0; step <= 100; ++step) {
    const float value = InterpolateBoundedHermite(
        -100.0f, 0.0f, 1.0f, 100.0f,
        0, kStep, kStep * 2, kStep * 3,
        static_cast<float>(step) / 100.0f);
    Expect(value >= -100.0f && value <= 100.0f,
           "bounded pose curve escaped its four-sample envelope");
  }
}

void TestPoseCurveHasContinuousSegmentVelocity() {
  using skate3::multiplayer::pose_curve::InterpolateHermite;

  constexpr std::uint64_t kStep = 16667;
  constexpr float kEpsilon = 0.001f;
  const float before_boundary = InterpolateHermite(
      0.0f, 1.0f, 4.0f, 9.0f,
      0, kStep, kStep * 2, kStep * 3,
      1.0f - kEpsilon);
  const float at_boundary = 4.0f;
  const float after_boundary = InterpolateHermite(
      1.0f, 4.0f, 9.0f, 16.0f,
      kStep, kStep * 2, kStep * 3, kStep * 4,
      kEpsilon);
  const float velocity_before =
      (at_boundary - before_boundary) / kEpsilon;
  const float velocity_after =
      (after_boundary - at_boundary) / kEpsilon;
  Expect(std::fabs(velocity_before - velocity_after) < 0.02f,
         "adjacent pose-curve segments changed velocity at a sample");
}

void TestMotionTraceMeasuresCadenceAndKeepsContinuity() {
  using skate3::multiplayer::motion::Snapshot;
  using skate3::multiplayer::motion::Window;

  Window window;
  const float first[3] = {0.0f, 0.0f, 0.0f};
  const float second[3] = {1.0f, 0.0f, 0.0f};
  const float third[3] = {2.0f, 0.0f, 0.0f};
  window.Record(1000000, first);
  window.Record(2000000, second);
  window.Record(3000000, third);
  window.Record(3000000, third);

  const Snapshot first_window = window.ReadAndReset();
  Expect(first_window.samples == 2,
         "motion trace counted a repeated timestamp");
  Expect(std::fabs(first_window.average_interval_ms - 1000.0) <
             0.001,
         "motion trace reported the wrong sample cadence");
  Expect(std::fabs(first_window.average_speed - 1.0) < 0.001,
         "motion trace reported the wrong constant speed");
  Expect(first_window.maximum_speed_change < 0.001,
         "motion trace invented variation at constant speed");

  const float fourth[3] = {4.0f, 0.0f, 0.0f};
  window.Record(4000000, fourth);
  const Snapshot second_window = window.ReadAndReset();
  Expect(second_window.samples == 1,
         "motion trace discarded continuity across log windows");
  Expect(std::fabs(second_window.average_speed - 2.0) < 0.001,
         "motion trace lost the next-window speed");
  Expect(std::fabs(second_window.maximum_speed_change - 1.0) <
             0.001,
         "motion trace lost cross-window speed variation");
}

void TestPoseCadenceMeasuresRepeatsAndAlternation() {
  using skate3::multiplayer::pose_cadence::Snapshot;
  using skate3::multiplayer::pose_cadence::Window;

  Window window;
  window.Record(1000000, 10, 0xAA);
  window.Record(1010000, 10, 0xAA);
  window.Record(1020000, 11, 0xBB);
  window.Record(1030000, 11, 0xAA);
  window.Record(1040000, 11, 0xAA);

  const Snapshot first = window.ReadAndReset();
  Expect(first.samples == 4,
         "pose cadence reported the wrong sample count");
  Expect(first.changes == 2 && first.repeats == 2,
         "pose cadence did not separate changed and repeated poses");
  Expect(first.sequence_changes == 1,
         "pose cadence reported the wrong source-sequence count");
  Expect(first.alternations == 1,
         "pose cadence missed an A/B/A palette alternation");
  Expect(first.maximum_repeat_run == 1 &&
             std::fabs(first.maximum_hold_ms - 10.0) < 0.001,
         "pose cadence reported the wrong repeated-pose hold");

  window.Record(1050000, 12, 0xCC);
  const Snapshot second = window.ReadAndReset();
  Expect(second.samples == 1 && second.changes == 1,
         "pose cadence discarded continuity across log windows");
  Expect(second.sequence_changes == 1,
         "pose cadence lost sequence continuity across log windows");
}

}  // namespace

int main() {
  TestLatestInputWins();
  TestImmutablePresentationAndSequences();
  TestEmptyPresentationAndClear();
  TestConcurrentHandoff();
  TestLatestRequestRejectsStalePublication();
  TestLatestRequestFailureAndGenerationForget();
  TestLatestRequestTransfersResultOwnership();
  TestPeriodicDeadlineRetainsTargetRate();
  TestPeriodicDeadlineDoesNotBurstAfterStall();
  TestAdaptiveInterpolationDelayCoversMeasuredStalls();
  TestPresentationClockRejectsCursorJumps();
  TestPresentationClockConvergesWithoutRewinding();
  TestPresentationClockSmoothsSteppedSceneAnchors();
  TestBoundedPoseCurvePreservesSamplesAndLimits();
  TestPoseCurveHasContinuousSegmentVelocity();
  TestMotionTraceMeasuresCadenceAndKeepsContinuity();
  TestPoseCadenceMeasuresRepeatsAndAlternation();

  if (g_failures != 0) {
    std::cerr << g_failures << " multiplayer worker test(s) failed\n";
    return 1;
  }
  std::cout << "All multiplayer worker tests passed\n";
  return 0;
}
