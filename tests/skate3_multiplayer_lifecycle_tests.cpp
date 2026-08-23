#include "skate3_multiplayer_lifecycle.h"

#include <chrono>
#include <iostream>
#include <string_view>

namespace {

using skate3::multiplayer::lifecycle::AppearanceAssemblyExpired;
using skate3::multiplayer::lifecycle::CanBeginAppearanceAssembly;
using skate3::multiplayer::lifecycle::CompleteAppearancePieceCount;
using skate3::multiplayer::lifecycle::kMaximumIncompleteAppearanceBytes;
using skate3::multiplayer::lifecycle::PeerGenerationTracker;
using skate3::multiplayer::protocol::kMaximumAppearanceBytes;

int g_failures = 0;

void Expect(bool condition, std::string_view message) {
  if (condition) {
    return;
  }
  std::cerr << "FAIL: " << message << '\n';
  ++g_failures;
}

void TestTransportIdentityReuse() {
  PeerGenerationTracker tracker;

  Expect(tracker.ObserveTransportIdentity(2, 1001),
         "first transport identity must start a generation");
  Expect(!tracker.ObserveTransportIdentity(2, 1001),
         "same Steam identity must not reset a generation every tick");
  Expect(tracker.ObserveTransportIdentity(2, 2002),
         "new Steam identity in the same role must reset the generation");
  Expect(tracker.size() == 1,
         "role reuse must replace rather than duplicate generation state");
}

void TestProcessSessionReuse() {
  PeerGenerationTracker tracker;

  Expect(tracker.ObserveProcessSession(7, 111),
         "first process session must start a generation");
  Expect(!tracker.ObserveProcessSession(7, 111),
         "same process session must remain stable");
  Expect(tracker.ObserveProcessSession(7, 222),
         "new process session in the same role must reset the generation");
}

void TestTransportReplacementInvalidatesProcessSession() {
  PeerGenerationTracker tracker;

  Expect(tracker.ObserveTransportIdentity(9, 1001),
         "initial transport identity was not observed");
  Expect(tracker.ObserveProcessSession(9, 333),
         "initial process session was not observed");
  Expect(tracker.ObserveTransportIdentity(9, 2002),
         "replacement transport identity was not observed");
  Expect(tracker.ObserveProcessSession(9, 333),
         "replacement transport must not inherit the old process session");
}

void TestDepartureAndRoleReuse() {
  PeerGenerationTracker tracker;

  Expect(tracker.ObserveTransportIdentity(3, 1001),
         "initial peer was not observed");
  Expect(tracker.Forget(3), "departed peer was not forgotten");
  Expect(!tracker.Forget(3), "forgetting an absent role should be stable");
  Expect(tracker.ObserveTransportIdentity(3, 1001),
         "a rejoined peer must start a fresh generation");
}

void TestInvalidIdentityDoesNotCreateState() {
  PeerGenerationTracker tracker;

  Expect(!tracker.ObserveTransportIdentity(0, 1001),
         "role zero must be rejected");
  Expect(!tracker.ObserveTransportIdentity(101, 1001),
         "roles above 100 must be rejected");
  Expect(!tracker.ObserveTransportIdentity(2, 0),
         "zero transport identity must be rejected");
  Expect(!tracker.ObserveProcessSession(2, 0),
         "zero process session must be rejected");
  Expect(tracker.size() == 0,
         "invalid observations must not allocate generation state");
}

void TestAppearanceAssemblyBudget() {
  Expect(!CanBeginAppearanceAssembly(0, 0),
         "empty appearance assemblies must be rejected");
  Expect(CanBeginAppearanceAssembly(0, kMaximumAppearanceBytes),
         "one maximum-size legacy appearance must fit the global budget");
  Expect(
      !CanBeginAppearanceAssembly(0, std::size_t{kMaximumAppearanceBytes} + 1),
      "per-peer appearance cap must be enforced");
  Expect(CanBeginAppearanceAssembly(kMaximumIncompleteAppearanceBytes -
                                        kMaximumAppearanceBytes,
                                    kMaximumAppearanceBytes),
         "an assembly exactly filling the global budget must be accepted");
  Expect(!CanBeginAppearanceAssembly(kMaximumIncompleteAppearanceBytes -
                                         kMaximumAppearanceBytes + 1,
                                     kMaximumAppearanceBytes),
         "an assembly exceeding the global budget by one byte must fail");
  Expect(!CanBeginAppearanceAssembly(kMaximumIncompleteAppearanceBytes + 1, 1),
         "an already-invalid global total must fail without underflow");
}

void TestAppearanceAssemblyTimeout() {
  using Clock = std::chrono::steady_clock;
  const Clock::time_point start = Clock::now();

  Expect(
      !AppearanceAssemblyExpired<Clock>(start + std::chrono::seconds(9), start),
      "active appearance assembly expired too early");
  Expect(
      AppearanceAssemblyExpired<Clock>(start + std::chrono::seconds(11), start),
      "stalled appearance assembly did not expire");
  Expect(!AppearanceAssemblyExpired<Clock>(start + std::chrono::seconds(30),
                                           Clock::time_point{}),
         "empty appearance assembly must not report a timeout");
}

void TestCompleteAppearancePieceCount() {
  Expect(CompleteAppearancePieceCount(7, 7),
         "complete appearance piece set was rejected");
  Expect(!CompleteAppearancePieceCount(7, 6),
         "partial appearance piece set was accepted");
  Expect(!CompleteAppearancePieceCount(0, 0),
         "empty appearance piece set was accepted");
}

} // namespace

int main() {
  TestTransportIdentityReuse();
  TestProcessSessionReuse();
  TestTransportReplacementInvalidatesProcessSession();
  TestDepartureAndRoleReuse();
  TestInvalidIdentityDoesNotCreateState();
  TestAppearanceAssemblyBudget();
  TestAppearanceAssemblyTimeout();
  TestCompleteAppearancePieceCount();

  if (g_failures != 0) {
    std::cerr << g_failures << " multiplayer lifecycle test(s) failed\n";
    return 1;
  }
  std::cout << "All multiplayer lifecycle tests passed\n";
  return 0;
}
