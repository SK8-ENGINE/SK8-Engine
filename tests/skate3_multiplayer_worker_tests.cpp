#include "skate3_multiplayer_worker.h"
#include "skate3_multiplayer_latest_request.h"
#include "skate3_multiplayer_send_schedule.h"

#include <atomic>
#include <cstdint>
#include <iostream>
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

  if (g_failures != 0) {
    std::cerr << g_failures << " multiplayer worker test(s) failed\n";
    return 1;
  }
  std::cout << "All multiplayer worker tests passed\n";
  return 0;
}
