#include "skate3_multiplayer_lifecycle.h"

#include <iostream>
#include <string_view>

namespace {

using skate3::multiplayer::lifecycle::PeerGenerationTracker;

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

} // namespace

int main() {
  TestTransportIdentityReuse();
  TestProcessSessionReuse();
  TestTransportReplacementInvalidatesProcessSession();
  TestDepartureAndRoleReuse();
  TestInvalidIdentityDoesNotCreateState();

  if (g_failures != 0) {
    std::cerr << g_failures << " multiplayer lifecycle test(s) failed\n";
    return 1;
  }
  std::cout << "All multiplayer lifecycle tests passed\n";
  return 0;
}
