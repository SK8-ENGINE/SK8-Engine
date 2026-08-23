#include "skate3_multiplayer_worker.h"

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

}  // namespace

int main() {
  TestLatestInputWins();
  TestImmutablePresentationAndSequences();
  TestEmptyPresentationAndClear();
  TestConcurrentHandoff();

  if (g_failures != 0) {
    std::cerr << g_failures << " multiplayer worker test(s) failed\n";
    return 1;
  }
  std::cout << "All multiplayer worker tests passed\n";
  return 0;
}
