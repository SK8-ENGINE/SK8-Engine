#include "skate3_multiplayer_protocol_v12.h"
#include "skate3_multiplayer_protocol_v12_state.h"

#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

using namespace skate3::multiplayer::protocol_v12;

int g_failures = 0;

void Expect(bool condition, std::string_view message) {
  if (condition) {
    return;
  }
  std::cerr << "FAIL: " << message << '\n';
  ++g_failures;
}

void TestReceiveHistoryLossReorderAndDuplicates() {
  ReceiveHistory history;
  Expect(history.Observe(100) == ReceiveDisposition::kNewLatest,
         "first packet was not the latest");
  Expect(history.latest() == 100 && history.history() == 0,
         "first packet initialized the wrong acknowledgement");
  Expect(history.Observe(103) == ReceiveDisposition::kNewLatest,
         "packet after a gap was not accepted");
  Expect(history.history() == (1u << 2),
         "gap did not preserve the old latest packet");
  Expect(history.Observe(101) == ReceiveDisposition::kReordered,
         "in-window reordered packet was not accepted");
  Expect(history.history() == ((1u << 2) | (1u << 1)),
         "reordered packet did not fill its history bit");
  Expect(history.Observe(101) == ReceiveDisposition::kDuplicate,
         "repeated reordered packet was not detected");
  Expect(history.Observe(70) == ReceiveDisposition::kTooOld,
         "packet outside receive history was accepted");
  Expect(SequenceAcknowledged(
             101, history.latest(), history.history()),
         "generated history did not acknowledge reordered packet");

  Expect(history.Observe(135) == ReceiveDisposition::kNewLatest,
         "32-packet advance was rejected");
  Expect(history.history() == (1u << 31),
         "32-packet advance did not retain only prior latest");
  Expect(history.Observe(200) == ReceiveDisposition::kNewLatest,
         "large sequence advance was rejected");
  Expect(history.history() == 0,
         "large sequence advance retained obsolete history");
}

void TestReceiveHistoryRollover() {
  ReceiveHistory history;
  Expect(history.Observe(UINT32_MAX - 1) ==
             ReceiveDisposition::kNewLatest,
         "rollover history did not initialize");
  Expect(history.Observe(1) == ReceiveDisposition::kNewLatest,
         "sequence rollover was not treated as newer");
  Expect(history.Observe(UINT32_MAX) ==
             ReceiveDisposition::kReordered,
         "reordered rollover packet was rejected");
  Expect(history.latest() == 1 &&
             SequenceAcknowledged(
                 UINT32_MAX, history.latest(), history.history()),
         "rollover packet was not represented in history");
  Expect(history.Observe(UINT32_MAX) ==
             ReceiveDisposition::kDuplicate,
         "rollover duplicate was not detected");
}

void TestCapabilityAcknowledgementTracksOutstandingAdvertisements() {
  CapabilityAcknowledgementState state;
  state.RecordSent(1);
  Expect(!state.Observe(0, 0),
         "unacknowledged capability advertisement activated v12");

  // Both peers may advertise in the same worker tick. The next advertisement
  // then acknowledges sequence 1 after sequence 2 has already been sent.
  state.RecordSent(2);
  Expect(state.Observe(1, 0),
         "one-behind lockstep acknowledgement did not activate v12");
  Expect(state.acknowledged(),
         "capability acknowledgement did not latch");

  state.Clear();
  state.RecordSent(10);
  state.RecordSent(11);
  Expect(state.Observe(11, 0),
         "newer delivered advertisement did not survive an earlier loss");
}

GenerationIdentity Generation(
    std::uint16_t role, std::uint32_t session) {
  return GenerationIdentity{
      .role = role,
      .session = session,
      .map_hash = 0x11,
      .build_hash = 0x22,
      .content_hash = 0x33,
  };
}

void TestValidatedGenerationActivation() {
  const ContentIdentity content{0x11, 0x22, 0x33};
  PeerGenerationState state;
  Expect(state.ActivateValidated(Generation(2, 10), content, 1) ==
             GenerationActivation::kFirst,
         "first compatible generation was not activated");
  Expect(state.Matches(2, 10),
         "active generation did not match realtime identity");
  Expect(state.ActivateValidated(Generation(2, 10), content, 1) ==
             GenerationActivation::kUnchanged,
         "same validated generation was not idempotent");
  Expect(state.ActivateValidated(Generation(2, 20), content, 1) ==
             GenerationActivation::kStale,
         "identity changed inside one transport generation");
  Expect(state.ActivateValidated(Generation(2, 20), content, 2) ==
             GenerationActivation::kReplaced,
         "new authenticated transport generation was not accepted");
  Expect(!state.Matches(2, 10) && state.Matches(2, 20),
         "replaced generation still accepted stale realtime packets");
  Expect(state.ActivateValidated(Generation(2, 10), content, 1) ==
             GenerationActivation::kStale,
         "old capability activation rolled generation backward");

  GenerationIdentity incompatible = Generation(2, 30);
  incompatible.content_hash = 0x99;
  Expect(state.ActivateValidated(incompatible, content, 3) ==
             GenerationActivation::kIncompatible,
         "incompatible content generation was accepted");
  Expect(state.Matches(2, 20),
         "incompatible activation replaced current generation");
  Expect(state.ActivateValidated(Generation(0, 30), content, 3) ==
             GenerationActivation::kInvalid,
         "invalid peer role was activated");
}

void TestGroupedBaselineRecovery() {
  constexpr std::uint32_t groups = 0b111u;
  PoseReceiverState receiver(groups);
  receiver.ActivateGeneration(3, 100);

  Expect(receiver.ReceiveDelta(3, 100, 1, 50, 0b001) ==
             PoseReceiveResult::kMissingBaseline,
         "delta without decoded baseline was accepted");
  Expect(receiver.baseline_request_pending(),
         "missing baseline did not schedule recovery");
  Expect(receiver.ConsumeBaselineRequest(),
         "scheduled baseline request was not consumable");
  Expect(receiver.baseline_recovery_active() &&
             !receiver.baseline_request_pending(),
         "consumed request did not enter bounded recovery");
  Expect(receiver.ReceiveDelta(3, 100, 2, 50, 0b010) ==
             PoseReceiveResult::kMissingBaseline,
         "second delta without baseline was accepted");
  Expect(!receiver.baseline_request_pending(),
         "recovery request was emitted for every missing delta");
  receiver.ScheduleBaselineRetry();
  Expect(receiver.baseline_request_pending(),
         "explicit recovery timeout did not schedule a retry");

  Expect(receiver.ReceiveBaselineGroup(3, 100, 3, 50, 0b001) ==
             PoseReceiveResult::kBaselineGroupAccepted,
         "first baseline group was not staged");
  Expect(receiver.ReceiveBaselineGroup(3, 100, 4, 50, 0b100) ==
             PoseReceiveResult::kBaselineGroupAccepted,
         "second baseline group was not staged");
  Expect(receiver.decoded_baseline_id() == 0,
         "incomplete grouped baseline was exposed to sender");
  Expect(receiver.ReceiveDelta(3, 100, 5, 50, 0b001) ==
             PoseReceiveResult::kMissingBaseline,
         "delta used an incomplete grouped baseline");
  Expect(receiver.ReceiveBaselineGroup(3, 100, 6, 50, 0b010) ==
             PoseReceiveResult::kBaselineCompleted,
         "final baseline group did not complete baseline");
  Expect(receiver.decoded_baseline_id() == 50 &&
             receiver.decoded_baseline_sequence() == 6,
         "completed baseline acknowledgement was wrong");
  Expect(!receiver.baseline_request_pending() &&
             !receiver.baseline_recovery_active(),
         "completed baseline did not clear recovery");
  Expect(receiver.ReceiveDelta(3, 100, 7, 50, 0b011) ==
             PoseReceiveResult::kDeltaAccepted,
         "delta against decoded baseline was rejected");
}

void TestPoseReorderGenerationAndValidation() {
  PoseReceiverState receiver(0b111u);
  receiver.ActivateGeneration(4, 200);
  Expect(receiver.ReceiveBaselineGroup(4, 200, 100, 10, 0b111) ==
             PoseReceiveResult::kBaselineCompleted,
         "complete one-packet baseline was rejected");
  Expect(receiver.ReceiveDelta(4, 200, 102, 10, 0b001) ==
             PoseReceiveResult::kDeltaAccepted,
         "newer matching delta was rejected");
  Expect(receiver.ReceiveDelta(4, 200, 101, 10, 0b010) ==
             PoseReceiveResult::kDeltaAccepted,
         "valid reordered delta was rejected");
  Expect(receiver.ReceiveDelta(4, 200, 101, 10, 0b010) ==
             PoseReceiveResult::kDuplicatePacket,
         "duplicate pose packet was not rejected");
  Expect(receiver.ReceiveDelta(4, 200, 10, 10, 0b010) ==
             PoseReceiveResult::kPacketTooOld,
         "obsolete pose packet was accepted");
  Expect(receiver.ReceiveDelta(4, 199, 103, 10, 0b001) ==
             PoseReceiveResult::kStaleGeneration,
         "stale session pose packet was accepted");
  Expect(receiver.packet_history().latest() == 102,
         "stale session packet polluted receive history");
  Expect(receiver.ReceiveDelta(4, 200, 103, 10, 0) ==
             PoseReceiveResult::kInvalidGroups,
         "empty pose group was accepted");
  Expect(receiver.packet_history().latest() == 102,
         "malformed group polluted receive history");
  Expect(receiver.ReceiveBaselineGroup(4, 200, 104, 9, 0b111) ==
             PoseReceiveResult::kStaleBaseline,
         "older baseline replaced decoded baseline");

  receiver.ActivateGeneration(4, 300);
  Expect(receiver.decoded_baseline_id() == 0 &&
             !receiver.packet_history().initialized(),
         "generation replacement retained old decode state");
  Expect(receiver.ReceiveDelta(4, 200, 1, 10, 0b001) ==
             PoseReceiveResult::kStaleGeneration,
         "old session survived generation replacement");
}

void TestFragmentReceiptIsSeparateFromGroupDecode() {
  PoseReceiverState receiver(0b11u);
  receiver.ActivateGeneration(5, 350);
  Expect(receiver.ReceivePoseFragment(5, 350, 10) ==
             PoseReceiveResult::kFragmentAccepted,
         "first baseline fragment was rejected");
  Expect(receiver.ReceivePoseFragment(5, 350, 12) ==
             PoseReceiveResult::kFragmentAccepted,
         "fragment after packet loss was rejected");
  Expect(receiver.ReceivePoseFragment(5, 350, 11) ==
             PoseReceiveResult::kFragmentAccepted,
         "reordered missing fragment was rejected");
  Expect(receiver.packet_history().latest() == 12 &&
             SequenceAcknowledged(
                 10,
                 receiver.packet_history().latest(),
                 receiver.packet_history().history()) &&
             SequenceAcknowledged(
                 11,
                 receiver.packet_history().latest(),
                 receiver.packet_history().history()),
         "fragment packets were not represented in receive history");
  Expect(receiver.decoded_baseline_id() == 0,
         "fragment receipt prematurely confirmed a baseline");

  Expect(receiver.CompleteBaselineGroup(
             5, 350, 12, 70, 0b01) ==
             PoseReceiveResult::kBaselineGroupAccepted,
         "decoded first baseline group was rejected");
  Expect(receiver.decoded_baseline_id() == 0,
         "partial decoded group set confirmed a baseline");
  Expect(receiver.ReceivePoseFragment(5, 350, 15) ==
             PoseReceiveResult::kFragmentAccepted,
         "final fragment for second baseline group was rejected");
  Expect(receiver.CompleteBaselineGroup(
             5, 350, 15, 70, 0b10) ==
             PoseReceiveResult::kBaselineCompleted,
         "decoded final baseline group did not confirm baseline");
  Expect(receiver.decoded_baseline_id() == 70 &&
             receiver.decoded_baseline_sequence() == 15,
         "baseline completion did not use decode completion point");
  Expect(receiver.CompleteDelta(5, 350, 70, 0b01) ==
             PoseReceiveResult::kDeltaAccepted,
         "decoded delta against confirmed baseline was rejected");
}

void TestBaselineReplacementAndRollover() {
  PoseReceiverState receiver(0b11u);
  receiver.ActivateGeneration(5, 400);
  Expect(receiver.ReceiveBaselineGroup(5, 400, 1, 20, 0b01) ==
             PoseReceiveResult::kBaselineGroupAccepted,
         "partial baseline was not staged");
  Expect(receiver.ReceiveBaselineGroup(5, 400, 2, 21, 0b10) ==
             PoseReceiveResult::kBaselineGroupAccepted,
         "newer baseline did not replace incomplete baseline");
  Expect(receiver.pending_group_mask() == 0b10,
         "new baseline inherited groups from obsolete baseline");
  Expect(receiver.ReceiveBaselineGroup(5, 400, 3, 20, 0b10) ==
             PoseReceiveResult::kStaleBaseline,
         "obsolete pending baseline resumed after replacement");
  Expect(receiver.ReceiveBaselineGroup(5, 400, 4, 21, 0b01) ==
             PoseReceiveResult::kBaselineCompleted,
         "replacement baseline did not complete");

  receiver.ActivateGeneration(5, 401);
  Expect(receiver.ReceiveBaselineGroup(
             5, 401, UINT32_MAX - 1, UINT32_MAX, 0b11) ==
             PoseReceiveResult::kBaselineCompleted,
         "baseline near rollover was rejected");
  Expect(receiver.ReceiveBaselineGroup(5, 401, 1, 1, 0b11) ==
             PoseReceiveResult::kBaselineCompleted,
         "new baseline across rollover was rejected");
  Expect(receiver.decoded_baseline_id() == 1,
         "baseline rollover installed the wrong generation");
}

void TestSenderUsesOnlyDecodedBaselineReports() {
  SenderBaselineState sender;
  sender.ActivateGeneration(500);
  Expect(sender.OfferBaseline(500, 20, 1000),
         "sender did not offer baseline");
  Expect(SequenceAcknowledged(1000, 1000, 0),
         "test packet acknowledgement was not valid");
  Expect(sender.confirmed_baseline_id() == 0,
         "packet acknowledgement implicitly confirmed decode");
  Expect(!sender.ConfirmDecodedBaseline(500, 19),
         "unknown decoded baseline report was accepted");
  Expect(sender.OfferBaseline(500, 21, 1010),
         "newer baseline offer was rejected");
  Expect(sender.ConfirmDecodedBaseline(500, 20),
         "delayed retained baseline report was rejected");
  Expect(sender.confirmed_baseline_id() == 20,
         "sender did not expose receiver-confirmed baseline");

  Expect(sender.OfferBaseline(500, 22, 1020),
         "second newer baseline offer was rejected");
  Expect(sender.confirmed_baseline_id() == 20,
         "new offer discarded still-usable confirmed baseline");
  Expect(!sender.ConfirmDecodedBaseline(499, 22),
         "stale generation confirmed a baseline");
  Expect(sender.confirmed_baseline_id() == 20,
         "stale generation changed confirmed baseline");
  Expect(sender.ConfirmDecodedBaseline(500, 22),
         "new decoded baseline report was rejected");
  Expect(sender.ConfirmDecodedBaseline(500, 21),
         "valid reordered baseline report was rejected");
  Expect(sender.confirmed_baseline_id() == 22,
         "reordered report rolled the confirmed baseline backward");

  sender.ActivateGeneration(600);
  Expect(sender.confirmed_baseline_id() == 0 &&
             sender.offered_baseline_id() == 0,
         "sender generation reset retained old baselines");
  Expect(!sender.ConfirmDecodedBaseline(500, 22),
         "old sender generation survived reset");

  SenderBaselineState bounded;
  bounded.ActivateGeneration(700);
  for (std::uint32_t baseline = 1;
       baseline <=
       SenderBaselineState::kMaximumRetainedOffers + 1;
       ++baseline) {
    Expect(bounded.OfferBaseline(
               700, baseline, 2000 + baseline),
           "bounded baseline history rejected monotonic offer");
  }
  Expect(!bounded.ConfirmDecodedBaseline(700, 1),
         "evicted baseline remained confirmable");
  Expect(bounded.ConfirmDecodedBaseline(700, 2),
         "oldest retained baseline was not confirmable");
}

}  // namespace

int main() {
  TestReceiveHistoryLossReorderAndDuplicates();
  TestReceiveHistoryRollover();
  TestCapabilityAcknowledgementTracksOutstandingAdvertisements();
  TestValidatedGenerationActivation();
  TestGroupedBaselineRecovery();
  TestPoseReorderGenerationAndValidation();
  TestFragmentReceiptIsSeparateFromGroupDecode();
  TestBaselineReplacementAndRollover();
  TestSenderUsesOnlyDecodedBaselineReports();

  if (g_failures != 0) {
    std::cerr << g_failures
              << " multiplayer protocol-v12 state test(s) failed\n";
    return 1;
  }
  std::cout
      << "All multiplayer protocol-v12 state tests passed\n";
  return 0;
}
