#include "skate3_multiplayer_capture.h"
#include "skate3_multiplayer_lifecycle.h"

#include <chrono>
#include <iostream>
#include <string_view>

namespace {

using skate3::multiplayer::lifecycle::AppearanceAssemblyExpired;
using skate3::multiplayer::lifecycle::AppearanceResendTargetRole;
using skate3::multiplayer::lifecycle::CanBeginAppearanceAssembly;
using skate3::multiplayer::lifecycle::CompleteAppearancePieceCount;
using skate3::multiplayer::lifecycle::kMaximumIncompleteAppearanceBytes;
using skate3::multiplayer::lifecycle::OutboundAppearanceState;
using skate3::multiplayer::lifecycle::PeerGenerationTracker;
using skate3::multiplayer::lifecycle::RemoteAppearanceRetirementMatches;
using skate3::multiplayer::lifecycle::RemoteAppearanceSessionChanged;
using skate3::multiplayer::lifecycle::RemoteAppearanceTextureStoreKey;
using skate3::multiplayer::capture::LocalPresentationPieceOwned;
using skate3::multiplayer::protocol::AppearanceDeliveryState;
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

void TestRemoteAppearanceTextureOwnership() {
  constexpr std::uint64_t content_key = 0x12345678ABCDEF01ull;
  constexpr std::uint64_t role_two_key =
      RemoteAppearanceTextureStoreKey(2, content_key);
  constexpr std::uint64_t role_three_key =
      RemoteAppearanceTextureStoreKey(3, content_key);

  Expect(role_two_key != 0,
         "valid remote texture key must not be empty");
  Expect(role_two_key ==
             RemoteAppearanceTextureStoreKey(2, content_key),
         "remote texture key must be deterministic");
  Expect(role_two_key != role_three_key,
         "two roles wearing one appearance must own separate textures");
  Expect(role_two_key !=
             RemoteAppearanceTextureStoreKey(2, content_key + 1),
         "one role's distinct texture content must use a distinct key");
  Expect(RemoteAppearanceTextureStoreKey(0, content_key) == 0,
         "role zero must not own a remote texture");
  Expect(RemoteAppearanceTextureStoreKey(101, content_key) == 0,
         "roles above 100 must not own a remote texture");
  Expect(RemoteAppearanceTextureStoreKey(2, 0) == 0,
         "empty content identity must not own a remote texture");
}

void TestRemoteAppearanceGenerationRetirement() {
  Expect(!RemoteAppearanceSessionChanged(111, 111),
         "stable process session must retain renderer resources");
  Expect(RemoteAppearanceSessionChanged(111, 222),
         "role reuse must release the previous session's resources");
  Expect(!RemoteAppearanceSessionChanged(0, 222),
         "an uninitialized renderer session must not look replaced");
  Expect(!RemoteAppearanceSessionChanged(111, 0),
         "an invalid observed session must not evict live resources");
  Expect(RemoteAppearanceRetirementMatches(111, 111),
         "matching retired generation must release renderer resources");
  Expect(!RemoteAppearanceRetirementMatches(222, 111),
         "stale retirement must not release a newer role occupant");
  Expect(!RemoteAppearanceRetirementMatches(0, 111),
         "uninitialized appearance must not match a retirement");
  Expect(!RemoteAppearanceRetirementMatches(111, 0),
         "invalid retirement must not release renderer resources");
}

void TestLocalPresentationCaptureOwnership() {
  Expect(LocalPresentationPieceOwned(
             0x1000, 0x1000, true, 400.0f, false),
         "verified local presentation was rejected after board separation");
  Expect(!LocalPresentationPieceOwned(
             0x1000, 0x2000, true, 1.0f, false),
         "nearby foreign presentation bypassed exact ownership");
  Expect(LocalPresentationPieceOwned(
             0, 0x2000, true, 16.0f, false),
         "near fallback presentation was rejected without an exact owner");
  Expect(!LocalPresentationPieceOwned(
             0, 0x2000, true, 16.01f, false),
         "far fallback presentation was accepted without board ownership");
  Expect(LocalPresentationPieceOwned(
             0, 0x2000, true, 400.0f, true),
         "verified detached board was rejected by fallback proximity");
  Expect(!LocalPresentationPieceOwned(
             0x1000, 0, true, 1.0f, false),
         "invalid presentation entity was accepted");
  Expect(!LocalPresentationPieceOwned(
             0x1000, 0x1000, false, 1.0f, false),
         "non-finite local presentation transform was accepted");
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

void TestAppearanceResendRouting() {
  Expect(AppearanceResendTargetRole(1, false, 3) == 3,
         "localhost host must resend directly to the requester");
  Expect(AppearanceResendTargetRole(2, false, 3) == 1,
         "localhost client must resend through the host relay");
  Expect(AppearanceResendTargetRole(2, true, 3) == 3,
         "Steam must resend directly to the requester");
  Expect(AppearanceResendTargetRole(2, false, 2) == 0,
         "self-requested appearance resend must be rejected");
  Expect(AppearanceResendTargetRole(0, false, 2) == 0,
         "invalid local role must not produce a resend target");
  Expect(AppearanceResendTargetRole(2, false, 101) == 0,
         "invalid requester role must not produce a resend target");
}

void TestAppearanceResendRestart() {
  using Clock = std::chrono::steady_clock;
  constexpr std::uint64_t identity = 0x12345678ABCDEF01ull;
  OutboundAppearanceState state;
  state.identity = identity;
  state.next_chunk = 7;
  state.completed_passes = 3;
  state.retry_after = Clock::now() + std::chrono::seconds(1);
  state.acknowledged_state = AppearanceDeliveryState::kInstalled;

  Expect(!state.RestartForRequest(identity + 1, identity),
         "stale appearance request restarted the current transfer");
  Expect(state.next_chunk == 7 && state.completed_passes == 3 &&
             state.acknowledged_state ==
                 AppearanceDeliveryState::kInstalled,
         "stale appearance request changed transfer progress");
  Expect(state.RestartForRequest(identity, identity),
         "matching appearance request did not restart the transfer");
  Expect(state.identity == identity && state.next_chunk == 0 &&
             state.completed_passes == 0 &&
             state.retry_after == Clock::time_point{} &&
             state.acknowledged_state ==
                 AppearanceDeliveryState::kUnknown,
         "appearance resend did not clear prior completion state");
}

} // namespace

int main() {
  TestTransportIdentityReuse();
  TestProcessSessionReuse();
  TestTransportReplacementInvalidatesProcessSession();
  TestDepartureAndRoleReuse();
  TestInvalidIdentityDoesNotCreateState();
  TestRemoteAppearanceTextureOwnership();
  TestRemoteAppearanceGenerationRetirement();
  TestLocalPresentationCaptureOwnership();
  TestAppearanceAssemblyBudget();
  TestAppearanceAssemblyTimeout();
  TestCompleteAppearancePieceCount();
  TestAppearanceResendRouting();
  TestAppearanceResendRestart();

  if (g_failures != 0) {
    std::cerr << g_failures << " multiplayer lifecycle test(s) failed\n";
    return 1;
  }
  std::cout << "All multiplayer lifecycle tests passed\n";
  return 0;
}
