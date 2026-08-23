#include "skate3_multiplayer_capture.h"
#include "skate3_multiplayer_lifecycle.h"

#include <chrono>
#include <iostream>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

using skate3::multiplayer::lifecycle::AppearanceAssemblyExpired;
using skate3::multiplayer::lifecycle::AppearanceResendTargetRole;
using skate3::multiplayer::lifecycle::CanBeginAppearanceAssembly;
using skate3::multiplayer::lifecycle::CompleteAppearancePieceCount;
using skate3::multiplayer::lifecycle::kMaximumIncompleteAppearanceBytes;
using skate3::multiplayer::lifecycle::
    LocalhostAppearanceFanoutRestartTarget;
using skate3::multiplayer::lifecycle::OutboundAppearanceState;
using skate3::multiplayer::lifecycle::PeerGenerationTracker;
using skate3::multiplayer::lifecycle::RemoteAppearanceRetirementMatches;
using skate3::multiplayer::lifecycle::RemoteAppearanceMeshKey;
using skate3::multiplayer::lifecycle::RemoteAppearanceTextureObjectKey;
using skate3::multiplayer::lifecycle::NextRemoteAppearanceResourceSlot;
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

void TestRemoteAppearanceStagingNamespaces() {
  Expect(NextRemoteAppearanceResourceSlot(false, 0) == 0,
         "first staged appearance must use slot zero");
  Expect(NextRemoteAppearanceResourceSlot(true, 0xE1000000u) == 1,
         "slot-zero appearance must stage its replacement in slot one");
  Expect(NextRemoteAppearanceResourceSlot(true, 0xE2000000u) == 0,
         "slot-one appearance must stage its replacement in slot zero");

  const std::uint32_t mesh_a = RemoteAppearanceMeshKey(0, 2, 3);
  const std::uint32_t mesh_b = RemoteAppearanceMeshKey(1, 2, 3);
  const std::uint32_t mesh_other_role =
      RemoteAppearanceMeshKey(0, 3, 3);
  Expect(mesh_a != 0 && mesh_b != 0,
         "valid staged mesh keys must not be empty");
  Expect(mesh_a != mesh_b,
         "two transactional slots must not share a mesh key");
  Expect(mesh_a != mesh_other_role,
         "two roles must not share a staged mesh key");

  const std::uint32_t texture_a =
      RemoteAppearanceTextureObjectKey(0, 2, 3);
  const std::uint32_t texture_b =
      RemoteAppearanceTextureObjectKey(1, 2, 3);
  Expect(texture_a != 0 && texture_a != texture_b,
         "two transactional slots must not share a texture object key");
  Expect(RemoteAppearanceMeshKey(2, 2, 3) == 0 &&
             RemoteAppearanceMeshKey(0, 0, 3) == 0 &&
             RemoteAppearanceTextureObjectKey(0, 101, 3) == 0,
         "invalid staging coordinates must not create renderer keys");
}

void TestRemoteAppearanceStagingChurn() {
  constexpr std::uint32_t role = 2;
  constexpr std::size_t piece_count = 11;
  constexpr std::size_t texture_count = 25;
  std::vector<std::uint32_t> current_meshes;
  std::vector<std::uint32_t> current_textures;
  std::unordered_set<std::uint32_t> live_meshes;
  std::unordered_set<std::uint32_t> live_textures;

  for (std::size_t change = 0; change < 128; ++change) {
    const std::uint8_t slot =
        NextRemoteAppearanceResourceSlot(
            !current_meshes.empty(),
            current_meshes.empty() ? 0 : current_meshes.front());
    std::vector<std::uint32_t> pending_meshes;
    std::vector<std::uint32_t> pending_textures;
    for (std::size_t piece = 0; piece < piece_count; ++piece) {
      const std::uint32_t key =
          RemoteAppearanceMeshKey(slot, role, piece);
      Expect(live_meshes.insert(key).second,
             "staged mesh overwrote a live appearance during churn");
      pending_meshes.push_back(key);
    }
    for (std::size_t texture = 0;
         texture < texture_count; ++texture) {
      const std::uint32_t key =
          RemoteAppearanceTextureObjectKey(
              slot, role, texture);
      Expect(live_textures.insert(key).second,
             "staged texture route overwrote a live appearance during churn");
      pending_textures.push_back(key);
    }

    for (std::uint32_t key : current_meshes) {
      Expect(live_meshes.erase(key) == 1,
             "committed churn left an old mesh unowned");
    }
    for (std::uint32_t key : current_textures) {
      Expect(live_textures.erase(key) == 1,
             "committed churn left an old texture route unowned");
    }
    current_meshes = std::move(pending_meshes);
    current_textures = std::move(pending_textures);
    Expect(live_meshes.size() == piece_count &&
               live_textures.size() == texture_count,
           "committed churn retained more than one appearance generation");
  }

  for (std::uint32_t key : current_meshes) {
    live_meshes.erase(key);
  }
  for (std::uint32_t key : current_textures) {
    live_textures.erase(key);
  }
  Expect(live_meshes.empty() && live_textures.empty(),
         "peer retirement did not return simulated resources to zero");
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

void TestLocalhostAppearanceFanoutRestart() {
  constexpr std::uint64_t identity = 0x12345678ABCDEF01ull;
  Expect(LocalhostAppearanceFanoutRestartTarget(
             2, false, 4, true, identity) == 1,
         "localhost client did not restart upstream appearance fanout "
         "for a late peer");
  Expect(LocalhostAppearanceFanoutRestartTarget(
             1, false, 4, true, identity) == 0,
         "localhost host incorrectly restarted an upstream fanout");
  Expect(LocalhostAppearanceFanoutRestartTarget(
             2, true, 4, true, identity) == 0,
         "Steam peer incorrectly used localhost fanout recovery");
  Expect(LocalhostAppearanceFanoutRestartTarget(
             2, false, 1, true, identity) == 0,
         "host capability advertisement restarted downstream fanout");
  Expect(LocalhostAppearanceFanoutRestartTarget(
             2, false, 4, false, identity) == 0,
         "stable peer capabilities repeatedly restarted appearance fanout");
  Expect(LocalhostAppearanceFanoutRestartTarget(
             2, false, 4, true, 0) == 0,
         "empty local appearance started a fanout stream");
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
  TestRemoteAppearanceStagingNamespaces();
  TestRemoteAppearanceStagingChurn();
  TestRemoteAppearanceGenerationRetirement();
  TestLocalPresentationCaptureOwnership();
  TestAppearanceAssemblyBudget();
  TestAppearanceAssemblyTimeout();
  TestCompleteAppearancePieceCount();
  TestAppearanceResendRouting();
  TestLocalhostAppearanceFanoutRestart();
  TestAppearanceResendRestart();

  if (g_failures != 0) {
    std::cerr << g_failures << " multiplayer lifecycle test(s) failed\n";
    return 1;
  }
  std::cout << "All multiplayer lifecycle tests passed\n";
  return 0;
}
