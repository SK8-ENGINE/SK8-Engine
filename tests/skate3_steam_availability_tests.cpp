#include "skate3_steam_availability.h"

#include <rex/ui/overlay/simple_multiplayer_availability.h>

#include <array>
#include <cstddef>
#include <iostream>
#include <string_view>

namespace {

using skate3::multiplayer::steam::availability::Tracker;
using skate3::multiplayer::steam::availability::Transition;

int g_failures = 0;

void Expect(bool condition, std::string_view message) {
  if (condition) {
    return;
  }
  std::cerr << "FAIL: " << message << '\n';
  ++g_failures;
}

class FakeSteamStatusProvider {
public:
  explicit FakeSteamStatusProvider(std::array<bool, 3> observations)
      : observations_(observations) {}

  [[nodiscard]] bool IntegrationReady() { return observations_[next_++]; }

private:
  std::array<bool, 3> observations_;
  std::size_t next_ = 0;
};

void TestUnavailableAvailableUnavailableTransitions() {
  FakeSteamStatusProvider provider({false, true, false});
  Tracker tracker;

  Expect(tracker.Observe(provider.IntegrationReady()) ==
             Transition::kBecameUnavailable,
         "initial unavailable observation must publish unavailable");
  Expect(!tracker.available(), "Steam controls must begin unavailable");
  Expect(!rex::ui::SimpleMultiplayerControlsEnabled(tracker.available()),
         "multiplayer UI must be disabled while integration is unavailable");

  Expect(tracker.Observe(provider.IntegrationReady()) ==
             Transition::kBecameAvailable,
         "successful integration must publish available");
  Expect(tracker.available(),
         "Steam controls must become available without a restart");
  Expect(rex::ui::SimpleMultiplayerControlsEnabled(tracker.available()),
         "multiplayer UI must enable after integration succeeds");

  Expect(tracker.Observe(provider.IntegrationReady()) ==
             Transition::kBecameUnavailable,
         "lost integration must publish unavailable");
  Expect(!tracker.available(),
         "Steam controls must become unavailable after service loss");
  Expect(!rex::ui::SimpleMultiplayerControlsEnabled(tracker.available()),
         "multiplayer UI must disable again after service loss");
}

void TestStableObservationsDoNotRepeatTransitions() {
  Tracker tracker;
  Expect(tracker.Observe(false) == Transition::kBecameUnavailable,
         "first unavailable observation must transition");
  Expect(tracker.Observe(false) == Transition::kNone,
         "stable unavailable state must not repeat a transition");
  Expect(tracker.Observe(true) == Transition::kBecameAvailable,
         "first available observation must transition");
  Expect(tracker.Observe(true) == Transition::kNone,
         "stable available state must not repeat a transition");
}

} // namespace

int main() {
  TestUnavailableAvailableUnavailableTransitions();
  TestStableObservationsDoNotRepeatTransitions();

  if (g_failures != 0) {
    std::cerr << g_failures << " Steam availability test(s) failed\n";
    return 1;
  }
  std::cout << "All Steam availability tests passed\n";
  return 0;
}
