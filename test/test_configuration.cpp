#include <miniraft/configuration.h>
#include <miniraft/durable.h>
#include <miniraft/raft.h>
#include <miniraft/persist.h>
#include <miniraft/snapshot_codec.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
extern "C" {
#include <cmocka.h>
}
namespace {
template<class F> void Reject(F f) {
    bool rejected = false;
    try { f(); } catch (const std::exception&) { rejected = true; }
    assert_true(rejected);
}
TConfigurationRecord Joint() {
    return {EConfigurationPhase::Joint, 3, 2, 2, {1, 2, 3}, {3, 4, 5}};
}
TMessageHolder<TLogEntry> Copy(TMessageHolder<TLogEntry> entry) {
    auto copy = NewHoldedMessage<TLogEntry>(entry->Len);
    memcpy(copy.Mes, entry.Mes, entry->Len); return copy;
}
void FixChecksum(TMessageHolder<TLogEntry> entry) {
    const auto length = entry->Len - sizeof(TLogEntry);
    const auto crc = NSnapshotCodec::Checksum(std::string_view(entry->Data, length - 4));
    for (int i = 0; i < 4; ++i) entry->Data[length - 4 + i] = char(crc >> (8 * i));
}
struct TEndpoint : INode {
    unsigned Sent = 0;
    std::vector<TMessageHolder<TMessage>> Messages;
    void Send(TMessageHolder<TMessage> message) override { ++Sent; Messages.push_back(std::move(message)); }
    void Drain() override {}
};
struct TCountingRsm : TDummyRsm {
    unsigned Writes = 0;
    TMessageHolder<TMessage> Write(TMessageHolder<TLogEntry> entry, uint64_t index) override {
        ++Writes;
        return TDummyRsm::Write(std::move(entry), index);
    }
};
TMessageHolder<TLogEntry> Ordinary(uint64_t term) {
    auto entry = NewHoldedMessage<TLogEntry>(); entry->Term = term; return entry;
}
void test_configuration_snapshot_projection_and_history_resume(void**) {
    auto initial = TConfigurationHistory::Stable({1, 2, 3}, 0, 0, 0, 0);
    Reject([&] { initial.MakeSnapshot("empty boundary"); });
    auto prefix = initial.Observe(1, Ordinary(1));
    const auto bootstrap = prefix.MakeSnapshot("bootstrap");
    assert_true(EncodeSnapshot(bootstrap).substr(0, 8) == "MRFTS001");
    assert_int_equal(bootstrap.Index, 1); assert_int_equal(bootstrap.Term, 1);
    assert_false(bootstrap.HasConfigurationMetadata());
    auto joint = Joint(); joint.Term = 2; joint.PreviousIndex = joint.PreviousTerm = 0;
    auto pending = prefix.Advance(2, EncodeConfigurationEntry(joint)).Observe(3, Ordinary(2));
    const auto pendingSnapshot = pending.MakeSnapshot(std::string("j\0x", 3));
    const auto jointBytes = EncodeSnapshot(pendingSnapshot);
    assert_true(jointBytes.substr(0, 8) == "MRFTS002");
    auto restoredJoint = TConfigurationHistory::FromJointSnapshot(DecodeSnapshot(jointBytes));
    assert_int_equal(restoredJoint.ThroughIndex(), 3); assert_int_equal(restoredJoint.ConfigurationIndex(), 2);
    assert_true(restoredJoint.OldVoters() == joint.OldVoters); assert_true(restoredJoint.NewVoters() == joint.NewVoters);
    auto final = joint; final.Phase = EConfigurationPhase::Stable; final.Term = 3;
    final.PreviousIndex = 2; final.PreviousTerm = 2;
    auto stable = restoredJoint.Advance(4, EncodeConfigurationEntry(final));
    const auto stableSnapshot = stable.MakeSnapshot("stable");
    const auto stableBytes = EncodeSnapshot(stableSnapshot);
    assert_true(stableBytes.substr(0, 8) == "MRFTS003");
    auto restoredStable = TConfigurationHistory::FromStableSnapshot(DecodeSnapshot(stableBytes));
    assert_int_equal(restoredStable.ConfigurationIndex(), 4); assert_int_equal(restoredStable.ConfigurationTerm(), 3);
    assert_true(restoredStable.OldVoters() == joint.NewVoters); assert_false(restoredStable.IsJoint());
    TConfigurationRecord next{EConfigurationPhase::Joint, 3, 4, 3, joint.NewVoters, {3, 4, 6}};
    auto resumed = restoredStable.Advance(5, EncodeConfigurationEntry(next));
    assert_true(resumed.IsJoint()); assert_true(resumed.NewVoters() == next.NewVoters);
    assert_true(pending.NewVoters() == joint.NewVoters); assert_false(stable.IsJoint());
    TState jointState; jointState.BindVoters(pendingSnapshot.Voters);
    jointState.CurrentTerm = pendingSnapshot.Term;
    jointState.InstallSnapshot(pendingSnapshot);
    assert_true(*jointState.GetSnapshot() == pendingSnapshot);
    TState stableState; stableState.BindVoters(stableSnapshot.Voters);
    stableState.CurrentTerm = stableSnapshot.Term;
    stableState.InstallSnapshot(stableSnapshot);
    assert_true(*stableState.GetSnapshot() == stableSnapshot);
    TState unbound; unbound.CurrentTerm = pendingSnapshot.Term;
    Reject([&] { unbound.InstallSnapshot(pendingSnapshot); });
    unbound.Snapshot = std::make_shared<TSnapshot>(pendingSnapshot);
    Reject([&] { unbound.BindVoters(pendingSnapshot.Voters); });
}
void test_history_validates_joint_stable_chain_and_independent_rollback(void**) {
    auto initial = TConfigurationHistory::Stable({1, 2, 3}, 0, 0, 0, 0);
    auto prefix = initial.Observe(1, Ordinary(1));
    auto joint = Joint(); joint.Term = 2; joint.PreviousIndex = joint.PreviousTerm = 0;
    auto pending = prefix.Advance(2, EncodeConfigurationEntry(joint));
    assert_true(pending.IsJoint());
    assert_true(pending.OldVoters() == joint.OldVoters); assert_true(pending.NewVoters() == joint.NewVoters);
    assert_int_equal(pending.ConfigurationIndex(), 2); assert_int_equal(pending.ConfigurationTerm(), 2);
    auto scanned = pending.Observe(3, Ordinary(2));
    auto final = joint; final.Phase = EConfigurationPhase::Stable; final.Term = 3;
    final.PreviousIndex = 2; final.PreviousTerm = 2;
    auto stable = scanned.Advance(4, EncodeConfigurationEntry(final));
    assert_false(stable.IsJoint()); assert_true(stable.OldVoters() == joint.NewVoters);
    assert_int_equal(stable.ConfigurationIndex(), 4); assert_int_equal(stable.ThroughTerm(), 3);
    // A replacement branch starts from the unchanged prior prefix.
    auto replacement = joint; replacement.Term = 3; replacement.NewVoters = {3, 5, 6};
    auto alternate = prefix.Advance(2, EncodeConfigurationEntry(replacement));
    assert_true(alternate.NewVoters() == replacement.NewVoters);
    assert_true(pending.NewVoters() == joint.NewVoters);
    assert_false(prefix.IsJoint()); assert_int_equal(prefix.ThroughIndex(), 1);
}
void test_configuration_scan_compares_full_configuration_state(void**) {
    auto committed = TConfigurationHistory::Stable({1, 2, 3}, 7, 3, 9, 4);
    auto differentVoters = TConfigurationHistory::Stable({1, 2, 4}, 7, 3, 9, 4);
    TConfigurationScan voterMismatch{committed, differentVoters};
    assert_true(voterMismatch.HasUncommittedConfiguration());

    TSnapshot jointSnapshot{9, 4, {1, 2, 3}, "opaque"};
    jointSnapshot.JointVoters = {3, 4, 5};
    jointSnapshot.ConfigurationIndex = 7; jointSnapshot.ConfigurationTerm = 3;
    auto joint = TConfigurationHistory::FromJointSnapshot(jointSnapshot);
    TConfigurationScan phaseMismatch{committed, joint};
    assert_true(phaseMismatch.HasUncommittedConfiguration());

    TConfigurationScan unchanged{committed, committed};
    assert_false(unchanged.HasUncommittedConfiguration());
}
void test_history_from_joint_snapshot_requires_a_consistent_identity(void**) {
    TSnapshot snapshot{8, 4, {1, 2, 3}, "opaque"};
    snapshot.JointVoters = {3, 4, 5}; snapshot.ConfigurationIndex = 5; snapshot.ConfigurationTerm = 3;
    auto history = TConfigurationHistory::FromJointSnapshot(snapshot);
    assert_true(history.IsJoint()); assert_int_equal(history.ConfigurationIndex(), 5);
    assert_int_equal(history.ConfigurationTerm(), 3); assert_int_equal(history.ThroughIndex(), 8);
    assert_int_equal(history.ThroughTerm(), 4);
    auto final = Joint(); final.Phase = EConfigurationPhase::Stable; final.Term = 4;
    final.PreviousIndex = 5; final.PreviousTerm = 3;
    auto stable = history.Advance(9, EncodeConfigurationEntry(final));
    assert_false(stable.IsJoint()); assert_true(stable.OldVoters() == final.NewVoters);
    auto fixed = snapshot; fixed.JointVoters.clear(); fixed.ConfigurationIndex = fixed.ConfigurationTerm = 0;
    Reject([&] { TConfigurationHistory::FromJointSnapshot(fixed); });
    auto inconsistent = snapshot; inconsistent.ConfigurationIndex = inconsistent.Index;
    Reject([&] { TConfigurationHistory::FromJointSnapshot(inconsistent); });
    Reject([&] { TConfigurationHistory::Stable({1}, 4, 2, 4, 3); });
    Reject([&] { TConfigurationHistory::Stable({1}, 5, 1, 4, 3); });
    Reject([&] { TConfigurationHistory::Stable({1}, 0, 0, 1, 0); });
}
void test_history_rejects_skips_wrong_predecessors_and_term_regression(void**) {
    auto base = TConfigurationHistory::Stable({1, 2, 3}, 0, 0, 0, 0);
    auto joint = Joint(); joint.Term = 2; joint.PreviousIndex = joint.PreviousTerm = 0;
    auto entry = EncodeConfigurationEntry(joint);
    Reject([&] { base.Advance(2, entry); });
    Reject([&] { base.Observe(2, Ordinary(1)); });
    Reject([&] { base.Observe(1, entry); });
    Reject([&] { base.Advance(1, Ordinary(1)); });
    auto wrong = joint; wrong.OldVoters = {1, 2, 4};
    Reject([&] { base.Advance(1, EncodeConfigurationEntry(wrong)); });
    auto prefix = base.Observe(1, Ordinary(3));
    Reject([&] { prefix.Advance(2, entry); });
    Reject([&] { prefix.Observe(2, Ordinary(2)); });
    auto valid = joint; valid.Term = 3;
    auto pending = prefix.Advance(2, EncodeConfigurationEntry(valid));
    wrong = valid; wrong.Phase = EConfigurationPhase::Stable; wrong.PreviousIndex = 2; wrong.PreviousTerm = 3;
    wrong.NewVoters = {3, 4, 6};
    Reject([&] { pending.Advance(3, EncodeConfigurationEntry(wrong)); });
    wrong = valid; wrong.PreviousIndex = 2; wrong.PreviousTerm = 3;
    Reject([&] { pending.Advance(3, EncodeConfigurationEntry(wrong)); });
    auto corrupt = EncodeConfigurationEntry(valid); corrupt->Data[0] ^= 1;
    Reject([&] { prefix.Advance(2, corrupt); });
    auto exhausted = TConfigurationHistory::Stable({1}, 0, 0, uint64_t(INT64_MAX), 1);
    Reject([&] { exhausted.Observe(UINT64_MAX, Ordinary(1)); });
    Reject([&] { exhausted.Advance(UINT64_MAX, entry); });
    assert_false(prefix.IsJoint()); assert_int_equal(prefix.ThroughIndex(), 1);
    assert_true(pending.IsJoint()); assert_int_equal(pending.ThroughIndex(), 2);
}
void test_scan_requires_each_configuration_predecessor_to_be_committed(void**) {
    TState state; state.CurrentTerm = 4;
    state.Append(Ordinary(1));
    const TConfigurationRecord joint{EConfigurationPhase::Joint, 2, 0, 0, {1}, {1, 2}};
    state.Append(EncodeConfigurationEntry(joint));
    const TConfigurationRecord stable{EConfigurationPhase::Stable, 3, 2, 2, {1}, {1, 2}};
    state.Append(EncodeConfigurationEntry(stable));
    const TConfigurationRecord nextJoint{EConfigurationPhase::Joint, 4, 3, 3, {1, 2}, {2, 3}};
    state.Append(EncodeConfigurationEntry(nextJoint));

    const auto baseline = TConfigurationHistory::Stable({1}, 0, 0, 0, 0);
    // The first joint entry is committed, but its stable successor is not.
    // The next joint transition must wait for that stable entry to commit.
    Reject([&] { ScanConfigurationHistory(state, baseline, 2); });

    const auto scan = ScanConfigurationHistory(state, baseline, 3);
    assert_false(scan.Committed.IsJoint());
    assert_true(scan.Committed.OldVoters() == stable.NewVoters);
    assert_true(scan.Latest.IsJoint());
    assert_true(scan.Latest.OldVoters() == nextJoint.OldVoters);
    assert_true(scan.Latest.NewVoters() == nextJoint.NewVoters);
    assert_true(scan.HasUncommittedConfiguration());
}
struct TCountingState : TState {
    mutable unsigned Gets = 0;
    TMessageHolder<TLogEntry> Get(int64_t index) const override { ++Gets; return TState::Get(index); }
};
struct TMismatchedMetadataState : TState {
    uint64_t RetainedLogTerm(int64_t index) const override { return TState::RetainedLogTerm(index) + 1; }
};
void test_scan_separates_commit_and_replaced_suffix(void**) {
    TCountingState state; state.CurrentTerm = 3;
    auto large = NewHoldedMessage<TLogEntry>(sizeof(TLogEntry) + 1024 * 1024); large->Term = 1;
    TConfigurationRecord joint{EConfigurationPhase::Joint, 2, 0, 0, {1}, {1, 2}};
    auto final = joint; final.Phase = EConfigurationPhase::Stable; final.Term = 3;
    final.PreviousIndex = 2; final.PreviousTerm = 2;
    state.Append(large); state.Append(EncodeConfigurationEntry(joint));
    state.Append(Ordinary(2)); state.Append(EncodeConfigurationEntry(final));
    auto base = TConfigurationHistory::Stable({1}, 0, 0, 0, 0);
    auto scan = ScanConfigurationHistory(state, base, 2);
    assert_true(scan.Committed.IsJoint()); assert_int_equal(scan.Committed.ThroughIndex(), 2);
    assert_false(scan.Latest.IsJoint()); assert_true(scan.Latest.OldVoters() == final.NewVoters);
    assert_true(scan.HasUncommittedConfiguration()); assert_int_equal(state.Gets, 2);
    const auto committedImage = scan.Committed.MakeSnapshot("committed image");
    assert_true(committedImage.IsJoint()); assert_int_equal(committedImage.Index, 2);
    assert_int_equal(committedImage.ConfigurationIndex, 2);
    scan = ScanConfigurationHistory(state, base, 4);
    assert_false(scan.HasUncommittedConfiguration()); assert_int_equal(scan.Committed.ThroughIndex(), 4);
    state.RemoveLast();
    scan = ScanConfigurationHistory(state, base, 2);
    assert_true(scan.Latest.IsJoint()); assert_false(scan.HasUncommittedConfiguration());
    state.Append(EncodeConfigurationEntry(final));
    Reject([&] { ScanConfigurationHistory(state, base, 1); });
    Reject([&] { base.ObserveMetadata(1, 1, TLogEntry::EConfiguration); });
    Reject([&] { base.ObserveMetadata(2, 1, 0); });
}
void test_scan_from_joint_snapshot_and_rejects_mismatched_baselines(void**) {
    TSnapshot snapshot{2, 2, {1, 2}, "image"};
    snapshot.JointVoters = {2, 3}; snapshot.ConfigurationIndex = 2; snapshot.ConfigurationTerm = 2;
    // The original binding is retained while the snapshot describes an active
    // configuration reached by an earlier committed transition.
    TState state; state.BindVoters({7, 8}); state.CurrentTerm = 2;
    state.InstallSnapshot(snapshot); state.CurrentTerm = 3; state.Append(Ordinary(2));
    TConfigurationRecord final{EConfigurationPhase::Stable, 3, 2, 2, {1, 2}, {2, 3}};
    state.Append(EncodeConfigurationEntry(final));
    auto base = TConfigurationHistory::FromJointSnapshot(snapshot);
    auto scan = ScanConfigurationHistory(state, base, 2);
    assert_true(scan.Committed.IsJoint()); assert_false(scan.Latest.IsJoint());
    assert_true(scan.Latest.OldVoters() == final.NewVoters); assert_true(scan.HasUncommittedConfiguration());
    assert_false(ScanConfigurationHistory(state, base, 4).HasUncommittedConfiguration());
    Reject([&] { ScanConfigurationHistory(state, base, 1); });
    Reject([&] { ScanConfigurationHistory(state, base, 5); });
    Reject([&] { ScanConfigurationHistory(state, TConfigurationHistory::Stable({1, 2}, 0, 0, 2, 2), 2); });
    auto wrong = snapshot; wrong.JointVoters = {2, 4};
    Reject([&] { ScanConfigurationHistory(state, TConfigurationHistory::FromJointSnapshot(wrong), 2); });
    state.BoundVoters = {1, 2, 4};
    assert_true(ScanConfigurationHistory(state, base, 2).Committed.IsJoint());
    state.BoundVoters = {7, 8};
    state.LastLogTerm = 2;
    Reject([&] { ScanConfigurationHistory(state, base, 2); });
}
void test_scan_from_stable_configuration_snapshot(void**) {
    TSnapshot snapshot{5, 2, {1, 2}, "image"}; snapshot.StableConfiguration = true;
    snapshot.ConfigurationIndex = 4; snapshot.ConfigurationTerm = 2;
    auto decoded = DecodeSnapshot(EncodeSnapshot(snapshot));
    auto baseline = TConfigurationHistory::FromStableSnapshot(decoded);
    assert_false(baseline.IsJoint()); assert_int_equal(baseline.ConfigurationIndex(), 4);
    TState state; state.BindVoters({7, 8});
    state.CurrentTerm = 2; state.InstallSnapshot(decoded);
    state.CurrentTerm = 3;
    TConfigurationRecord joint{EConfigurationPhase::Joint, 3, 4, 2, {1, 2}, {2, 3}};
    state.Append(EncodeConfigurationEntry(joint));
    auto scan = ScanConfigurationHistory(state, baseline, 5);
    assert_false(scan.Committed.IsJoint()); assert_true(scan.Latest.IsJoint());
    assert_true(scan.HasUncommittedConfiguration());
    auto wrong = TConfigurationHistory::Stable({1, 2}, 0, 0, 5, 2);
    Reject([&] { ScanConfigurationHistory(state, wrong, 5); });
    wrong = TConfigurationHistory::Stable({1, 2}, 4, 1, 5, 2);
    Reject([&] { ScanConfigurationHistory(state, wrong, 5); });
    Reject([&] { TConfigurationHistory::FromJointSnapshot(decoded); });
    Reject([&] { TConfigurationHistory::FromStableSnapshot(TSnapshot{5, 2, {1, 2}, "legacy"}); });
    Reject([&] { state.InstallSnapshot(snapshot); });
    auto wrongPredecessor = joint; wrongPredecessor.PreviousIndex = 3;
    state.RemoveLast(); state.Append(EncodeConfigurationEntry(wrongPredecessor));
    Reject([&] { ScanConfigurationHistory(state, baseline, 5); });
}
void test_scan_fails_closed_on_invalid_suffix(void**) {
    TState state; state.CurrentTerm = 3;
    auto base = TConfigurationHistory::Stable({1}, 0, 0, 0, 0);
    state.Append(Ordinary(2));
    TConfigurationRecord wrong{EConfigurationPhase::Joint, 3, 1, 2, {1}, {1, 2}};
    state.Append(EncodeConfigurationEntry(wrong));
    const auto fields = static_cast<TStateFields>(state);
    Reject([&] { ScanConfigurationHistory(state, base, 1); });
    assert_true(static_cast<TStateFields>(state) == fields); assert_int_equal(base.ThroughIndex(), 0);
    state.RemoveLast(); state.Append(Ordinary(1));
    Reject([&] { ScanConfigurationHistory(state, base, 1); });
    state.RemoveLast(); state.Append(Ordinary(3));
    state.BoundVoters = {2};
    Reject([&] { ScanConfigurationHistory(state, base, 1); });
    state.BoundVoters.clear(); state.CurrentTerm = 2;
    Reject([&] { ScanConfigurationHistory(state, base, 1); });
    TState corrupt; corrupt.CurrentTerm = 3;
    TConfigurationRecord joint{EConfigurationPhase::Joint, 2, 0, 0, {1}, {1, 2}};
    auto damaged = EncodeConfigurationEntry(joint); damaged->Data[0] ^= 1;
    corrupt.Append(damaged);
    Reject([&] { ScanConfigurationHistory(corrupt, base, 0); });
    TMismatchedMetadataState mismatch; mismatch.CurrentTerm = 3;
    mismatch.Append(EncodeConfigurationEntry(joint));
    Reject([&] { ScanConfigurationHistory(mismatch, base, 0); });
}
void test_configuration_roundtrips_both_phases_and_bounded_maximum(void**) {
    auto record = Joint();
    for (auto phase : {EConfigurationPhase::Joint, EConfigurationPhase::Stable}) {
        record.Phase = phase;
        auto entry = EncodeConfigurationEntry(record);
        assert_int_equal(entry->Flags, TLogEntry::EConfiguration);
        assert_true(DecodeConfigurationEntry(entry) == record);
    }
    record = {EConfigurationPhase::Joint, 1, 0, 0, {1}, {1, 2}};
    assert_true(DecodeConfigurationEntry(EncodeConfigurationEntry(record)) == record);
    record.OldVoters.clear(); record.NewVoters.clear();
    for (uint32_t id = 1; id <= TSnapshot::MaxVoters; ++id) {
        record.OldVoters.push_back(id); record.NewVoters.push_back(id + TSnapshot::MaxVoters);
    }
    auto max = EncodeConfigurationEntry(record);
    assert_int_equal(max->Len, sizeof(TLogEntry) + 48 + 8 * TSnapshot::MaxVoters);
    assert_true(DecodeConfigurationEntry(max) == record);
}
void test_configuration_rejects_invalid_fields_and_valid_crc_corruption(void**) {
    for (int mode = 0; mode < 12; ++mode) {
        auto record = Joint();
        if (mode == 0) record.Phase = EConfigurationPhase(0);
        if (mode == 1) record.Term = 0;
        if (mode == 2) record.PreviousIndex = UINT64_MAX;
        if (mode == 3) record.PreviousIndex = 0;
        if (mode == 4) record.PreviousTerm = 0;
        if (mode == 5) record.PreviousTerm = 4;
        if (mode == 6) record.OldVoters.clear();
        if (mode == 7) record.NewVoters = {0, 1};
        if (mode == 8) record.NewVoters = {1, 1};
        if (mode == 9) record.NewVoters = {3, 2};
        if (mode == 10) { record.Phase = EConfigurationPhase::Stable; record.PreviousIndex = record.PreviousTerm = 0; }
        if (mode == 11) record.OldVoters.resize(TSnapshot::MaxVoters + 1, 1);
        Reject([&] { EncodeConfigurationEntry(record); });
    }
    auto original = EncodeConfigurationEntry(Joint());
    const auto payload = original->Len - sizeof(TLogEntry);
    for (size_t i = 0; i < payload; ++i) {
        auto bad = Copy(original); bad->Data[i] ^= 1;
        Reject([&] { DecodeConfigurationEntry(bad); });
        bad = Copy(original); bad->Len = sizeof(TLogEntry) + i;
        Reject([&] { DecodeConfigurationEntry(bad); });
    }
    for (size_t offset : {size_t(8), size_t(12), size_t(20), size_t(28), size_t(36), size_t(40), size_t(44), size_t(56)}) {
        auto bad = Copy(original);
        const auto width = offset == 12 || offset == 20 || offset == 28 ? 8 : 4;
        for (size_t i = offset; i < offset + width; ++i) bad->Data[i] = offset >= 44 ? 0 : char(0xff);
        FixChecksum(bad); Reject([&] { DecodeConfigurationEntry(bad); });
    }
    auto bad = Copy(original); bad->Term++; Reject([&] { DecodeConfigurationEntry(bad); });
    bad = Copy(original); bad->Flags |= TLogEntry::EStub; Reject([&] { DecodeConfigurationEntry(bad); });
    bad = Copy(original); bad->Len += 1; Reject([&] { DecodeConfigurationEntry(bad); });
    bad = Copy(original); bad.InitPayload(1); Reject([&] { DecodeConfigurationEntry(bad); });
}
void test_configuration_rpc_and_proposal_are_rejected_before_mutation(void**) {
    auto peer = std::make_shared<TEndpoint>(); auto state = std::make_shared<TState>();
    TRaft follower(state, 2, {{1, peer}});
    auto record = Joint(); record.Term = 2; record.PreviousIndex = record.PreviousTerm = 0;
    auto config = EncodeConfigurationEntry(record);
    auto request = NewHoldedMessage(TMessageEx{.Src = 1, .Dst = 2, .Term = 2},
        TAppendEntriesRequest{.LeaderId = 1, .Nentries = 1});
    request.InitPayload(1); request.Payload[0] = config;
    const auto before = static_cast<TStateFields>(*state);
    follower.Process({}, request);
    assert_true(static_cast<TStateFields>(*state) == before); assert_int_equal(peer->Sent, 0);
    config->Flags |= TLogEntry::EStub; follower.Process({}, request);
    assert_true(static_cast<TStateFields>(*state) == before); assert_int_equal(peer->Sent, 0);
    auto normal = NewHoldedMessage<TLogEntry>(); normal->Term = 2; request.Payload[0] = normal;
    follower.Process({}, request); assert_int_equal(state->CurrentTerm, 2); assert_int_equal(state->LastLogIndex, 1);
    assert_int_equal(peer->Sent, 1);
    auto leaderState = std::make_shared<TState>(); TRaft leader(leaderState, 1, {});
    leader.ProcessTimeout({}); assert_int_equal(leader.CurrentStateName(), EState::LEADER);
    const auto leaderBefore = static_cast<TStateFields>(*leaderState);
    auto proposed = EncodeConfigurationEntry(record);
    Reject([&] { leader.Append(proposed); });
    assert_true(static_cast<TStateFields>(*leaderState) == leaderBefore); assert_int_equal(proposed->Term, 2);
}
void test_configuration_application_advances_without_calling_state_machine(void**) {
    auto state = std::make_shared<TState>(); auto record = Joint(); record.Term = 1;
    record.PreviousIndex = record.PreviousTerm = 0;
    state->Append(EncodeConfigurationEntry(record));
    Reject([&] { TRaft invalid(state, 1, {}); });
    assert_true(state->GetBoundVoters().empty());
    auto live = std::make_shared<TState>(); auto raft = std::make_shared<TRaft>(live, 1, TNodeDict{});
    auto app = std::make_shared<TCountingRsm>(); TRequestProcessor processor(raft, app, {});
    raft->ProcessTimeout({});
    processor.ProcessCommitted();
    assert_int_equal(app->LastAppliedIndex, 1); // Leader's initial stub.
    record.Term = live->CurrentTerm;
    live->Append(EncodeConfigurationEntry(record));
    raft->ProcessTimeout(ITimeSource::Time{} + std::chrono::seconds(1));
    processor.ProcessCommitted();
    assert_int_equal(app->LastAppliedIndex, 2);
    assert_int_equal(app->Writes, 0);
    auto compacted = std::make_shared<TState>();
    { TRaft bootstrap(compacted, 1, {}); }
    compacted->Append(NewHoldedMessage<TLogEntry>());
    compacted->InstallSnapshot({1, 1, {1}, "fixed image"});
    compacted->Append(EncodeConfigurationEntry(record));
    const auto fields = static_cast<TStateFields>(*compacted);
    Reject([&] { TRaft invalid(compacted, 1, {}); });
    assert_true(static_cast<TStateFields>(*compacted) == fields);
    assert_true(compacted->GetSnapshot()->Data == "fixed image");
}
#ifdef __linux__
void test_durable_restart_rejects_reserved_entry_without_rewriting(void**) {
    auto dir = std::filesystem::temp_directory_path() / ("miniraft-config-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    assert_true(std::filesystem::create_directory(dir));
    const auto name = (dir / "raft").string(); const auto path = name + ".durable.1";
    auto record = Joint(); record.Term = 1; record.PreviousIndex = record.PreviousTerm = 0;
    { auto state = std::make_shared<TDurableState>(name, 1);
      { TRaft bootstrap(state, 1, {}); }
      state->Append(EncodeConfigurationEntry(record)); }
    std::ifstream input(path, std::ios::binary); std::string before(std::istreambuf_iterator<char>(input), {}); input.close();
    { auto state = std::make_shared<TDurableState>(name, 1);
      const auto fields = static_cast<TStateFields>(*state);
      Reject([&] { TRaft invalid(state, 1, {}); });
      assert_true(static_cast<TStateFields>(*state) == fields); }
    std::ifstream after(path, std::ios::binary);
    assert_true(std::string(std::istreambuf_iterator<char>(after), {}) == before);
    std::error_code ignored; std::filesystem::remove_all(dir, ignored);
}
void test_legacy_restart_uses_safe_flag_lookup_and_preserves_files(void**) {
    auto dir = std::filesystem::temp_directory_path() / ("miniraft-legacy-config-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    assert_true(std::filesystem::create_directory(dir));
    const auto name = (dir / "raft").string();
    auto record = Joint(); record.Term = 1; record.PreviousIndex = record.PreviousTerm = 0;
    { TDiskState state(name, 2); state.Append(EncodeConfigurationEntry(record)); }
    std::vector<std::string> before;
    for (auto suffix : {".entries.2", ".index.2", ".state.2"}) {
        std::ifstream input(name + suffix, std::ios::binary);
        before.emplace_back(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    { auto state = std::make_shared<TDiskState>(name, 2);
      const auto fields = static_cast<TStateFields>(*state);
      Reject([&] { TRaft invalid(state, 2, {}); });
      assert_true(static_cast<TStateFields>(*state) == fields); }
    size_t i = 0;
    for (auto suffix : {".entries.2", ".index.2", ".state.2"}) {
        std::ifstream input(name + suffix, std::ios::binary);
        assert_true(std::string(std::istreambuf_iterator<char>(input), {}) == before[i++]);
    }
    std::error_code ignored; std::filesystem::remove_all(dir, ignored);
}
void test_offline_history_scan_after_durable_snapshot_and_restart(void**) {
    auto dir = std::filesystem::temp_directory_path() / ("miniraft-history-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    assert_true(std::filesystem::create_directory(dir));
    const auto name = (dir / "raft").string(); const auto path = name + ".durable.1";
    TConfigurationRecord joint{EConfigurationPhase::Joint, 2, 0, 0, {1}, {1, 2}};
    auto final = joint; final.Phase = EConfigurationPhase::Stable; final.Term = 3;
    final.PreviousIndex = 2; final.PreviousTerm = 2;
    {
        auto state = std::make_shared<TDurableState>(name, 1);
        { TRaft bootstrap(state, 1, {}); }
        state->Append(Ordinary(1)); state->InstallSnapshot({1, 1, {1}, "fixed image"});
        state->CurrentTerm = 3; state->Commit();
        state->Append(EncodeConfigurationEntry(joint)); state->Append(Ordinary(2));
        state->Append(EncodeConfigurationEntry(final));
    }
    std::ifstream input(path, std::ios::binary); std::string before(std::istreambuf_iterator<char>(input), {}); input.close();
    {
        auto state = std::make_shared<TDurableState>(name, 1);
        // This fixture explicitly knows that index 1 precedes any config entry.
        auto history = TConfigurationHistory::Stable({1}, 0, 0, state->SnapshotIndex, state->SnapshotTerm);
        const auto scan = ScanConfigurationHistory(*state, history, 2);
        assert_true(scan.Committed.IsJoint()); assert_false(scan.Latest.IsJoint());
        assert_true(scan.Latest.OldVoters() == final.NewVoters);
        assert_true(scan.HasUncommittedConfiguration());
        history = history.Advance(2, state->Get(2));
        assert_true(history.IsJoint());
        history = history.Observe(3, state->Get(3));
        history = history.Advance(4, state->Get(4));
        assert_false(history.IsJoint()); assert_true(history.OldVoters() == final.NewVoters);
        assert_int_equal(history.ConfigurationIndex(), 4);
        Reject([&] { TRaft disabled(state, 1, {}); });
        assert_true(state->GetSnapshot()->Data == "fixed image");
    }
    std::ifstream after(path, std::ios::binary);
    assert_true(std::string(std::istreambuf_iterator<char>(after), {}) == before);
    std::error_code ignored; std::filesystem::remove_all(dir, ignored);
}
void test_raft_recovers_joint_quorum_for_candidate_election(void**) {
    auto dir = std::filesystem::temp_directory_path() / ("miniraft-joint-quorum-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    assert_true(std::filesystem::create_directory(dir));
    const auto name = (dir / "raft").string();
    {
        auto state = std::make_shared<TDurableState>(name, 4);
        state->BindVoters({1, 2, 3});
        state->CurrentTerm = 2;
        state->Commit();
        TConfigurationRecord joint{EConfigurationPhase::Joint, 2, 0, 0, {1, 2, 3}, {3, 4, 5}};
        state->Append(EncodeConfigurationEntry(joint));
        state->Commit();

        TNodeDict peers;
        std::unordered_map<uint32_t, std::shared_ptr<TEndpoint>> endpoints;
        for (uint32_t id : {1, 2, 3, 5}) {
            auto endpoint = std::make_shared<TEndpoint>();
            endpoints.emplace(id, endpoint);
            peers.emplace(id, endpoint);
        }
        auto raft = std::make_shared<TRaft>(state, 4, peers, std::vector<uint32_t>{4, 5});
        assert_true(raft->IsVoter(4));
        assert_true(raft->IsVoter(5));
        assert_int_equal(raft->GetNservers(), 5);

        auto now = ITimeSource::Time{};
        raft->ProcessTimeout(now); // self vote alone cannot satisfy either joint majority
        assert_true(raft->CurrentStateName() == EState::CANDIDATE);
        auto vote = [&](uint32_t source) {
            raft->Process(now, NewHoldedMessage(TMessageEx{.Src = source, .Dst = 4, .Term = 3},
                TRequestVoteResponse{.VoteGranted = 1}));
            raft->ProcessTimeout(now + std::chrono::milliseconds(1));
        };

        vote(1); // This leaves both voter sets below their strict majority.
        assert_true(raft->CurrentStateName() == EState::CANDIDATE);
        vote(3); // 3 plus self satisfies new, while 1 plus 3 satisfies old.
        assert_true(raft->CurrentStateName() == EState::LEADER);

        raft->ProcessTimeout(now + std::chrono::milliseconds(2));
        auto ack = [&](uint32_t source, uint64_t expectedIndex, int responseMillis) {
            auto request = endpoints.at(source)->Messages.back().Cast<TAppendEntriesRequest>();
            const auto matchIndex = request->PrevLogIndex + request->Nentries;
            assert_int_equal(matchIndex, expectedIndex);
            raft->Process(now + std::chrono::milliseconds(responseMillis),
                NewHoldedMessage(TMessageEx{.Src = source, .Dst = 4, .Term = 3, .Seqno = request->Seqno},
                    TAppendEntriesResponse{.MatchIndex = matchIndex, .Success = 1}));
        };
        ack(3, 1, 3);
        ack(5, 1, 3);
        ack(1, 1, 3);
        assert_int_equal(raft->GetVolatileState()->CommitIndex, 0);

        raft->ProcessTimeout(now + std::chrono::milliseconds(4));
        ack(3, 2, 5);
        assert_int_equal(raft->GetVolatileState()->CommitIndex, 0);
        ack(5, 2, 5);
        assert_int_equal(raft->GetVolatileState()->CommitIndex, 0);
        assert_int_equal(raft->CommitSeqno(), 0);
        ack(1, 2, 5);
        assert_int_equal(raft->GetVolatileState()->CommitIndex, 2);
        assert_true(raft->CommitSeqno() > 0);
    }
    std::error_code ignored; std::filesystem::remove_all(dir, ignored);
}
void test_raft_keeps_joint_quorum_until_stable_successor_commits(void**) {
    auto dir = std::filesystem::temp_directory_path() / ("miniraft-joint-finalize-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    assert_true(std::filesystem::create_directory(dir));
    auto state = std::make_shared<TDurableState>((dir / "raft").string(), 4);
    state->BindVoters({1, 2, 3});
    state->CurrentTerm = 3;
    state->Commit();
    TConfigurationRecord joint{EConfigurationPhase::Joint, 2, 0, 0, {1, 2, 3}, {3, 4, 5}};
    state->Append(EncodeConfigurationEntry(joint));
    auto stable = joint;
    stable.Phase = EConfigurationPhase::Stable;
    stable.Term = 3;
    stable.PreviousIndex = 1;
    stable.PreviousTerm = 2;
    state->Append(EncodeConfigurationEntry(stable));
    state->CommitIndex = 1;
    state->Commit();

    TNodeDict peers;
    for (uint32_t id : {1, 2, 3, 5}) peers.emplace(id, std::make_shared<TEndpoint>());
    {
        TRaft inJoint(state, 4, peers, std::vector<uint32_t>{4, 5});
        assert_true(inJoint.IsVoter(1));
        assert_true(inJoint.IsVoter(5));
        assert_int_equal(inJoint.GetNservers(), 5);
    }

    state->CommitIndex = 2;
    state->Commit();
    {
        TRaft afterStableCommit(state, 4, peers, std::vector<uint32_t>{4, 5});
        assert_false(afterStableCommit.IsVoter(1));
        assert_false(afterStableCommit.IsVoter(2));
        assert_true(afterStableCommit.IsVoter(3));
        assert_true(afterStableCommit.IsVoter(5));
        assert_int_equal(afterStableCommit.GetNservers(), 3);
    }
    state.reset();
    std::error_code ignored; std::filesystem::remove_all(dir, ignored);
}
void test_raft_rejects_stable_snapshot_that_changes_active_voters(void**) {
    auto state = std::make_shared<TState>();
    state->BindVoters({1, 2, 3});
    state->CurrentTerm = 5;
    TSnapshot snapshot{5, 4, {2, 3, 4}, "stable image"};
    snapshot.StableConfiguration = true;
    snapshot.ConfigurationIndex = 4;
    snapshot.ConfigurationTerm = 3;
    state->InstallSnapshot(snapshot);

    TNodeDict peers{
        {3, std::make_shared<TEndpoint>()},
        {4, std::make_shared<TEndpoint>()},
    };
    const auto fields = static_cast<TStateFields>(*state);
    Reject([&] { TRaft raft(state, 2, peers); });
    assert_true(static_cast<TStateFields>(*state) == fields);
    assert_true(state->GetBoundVoters() == std::vector<uint32_t>({1, 2, 3}));
}

void test_persisted_commit_boundary_drives_committed_configuration_scan(void**) {
    auto dir = std::filesystem::temp_directory_path() / ("miniraft-committed-history-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    assert_true(std::filesystem::create_directory(dir));
    const auto name = (dir / "raft").string();
    TConfigurationRecord joint{EConfigurationPhase::Joint, 2, 0, 0, {1}, {1, 2}};
    auto final = joint; final.Phase = EConfigurationPhase::Stable; final.Term = 3;
    final.PreviousIndex = 2; final.PreviousTerm = 2;
    {
        auto state = std::make_shared<TDurableState>(name, 1);
        { TRaft bootstrap(state, 1, {}); }
        state->CurrentTerm = 3; state->Commit();
        state->Append(Ordinary(1));
        state->Append(EncodeConfigurationEntry(joint));
        state->Append(Ordinary(2));
        state->Append(EncodeConfigurationEntry(final));
        state->CommitIndex = 3; state->Commit();
    }
    {
        TDurableState state(name, 1);
        auto baseline = TConfigurationHistory::Stable({1}, 0, 0, 0, 0);
        const auto scan = ScanCommittedConfigurationHistory(state, baseline);
        assert_int_equal(state.CommitIndex, 3);
        assert_true(scan.Committed.IsJoint());
        assert_int_equal(scan.Committed.ConfigurationIndex(), 2);
        assert_false(scan.Latest.IsJoint());
        assert_true(scan.Latest.OldVoters() == final.NewVoters);
        assert_true(scan.HasUncommittedConfiguration());
        Reject([&] {
            auto wrong = TConfigurationHistory::Stable({2}, 0, 0, 0, 0);
            ScanCommittedConfigurationHistory(state, wrong);
        });
    }
    TState volatileState; volatileState.BindVoters({1});
    Reject([&] {
        auto baseline = TConfigurationHistory::Stable({1}, 0, 0, 0, 0);
        ScanCommittedConfigurationHistory(volatileState, baseline);
    });
    std::error_code ignored; std::filesystem::remove_all(dir, ignored);
}

void test_configuration_snapshots_preserve_original_binding_across_transitions(void**) {
    auto dir = std::filesystem::temp_directory_path() / ("miniraft-config-snapshot-binding-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    assert_true(std::filesystem::create_directory(dir));
    const auto name = (dir / "raft").string();
    const std::vector<uint32_t> original{1, 2, 3};
    const std::vector<uint32_t> firstActive{2, 3, 4};
    const std::vector<uint32_t> secondActive{2, 4, 5};
    const std::vector<uint32_t> thirdActive{4, 5, 6};

    TSnapshot jointSnapshot{5, 4, firstActive, "joint image"};
    jointSnapshot.JointVoters = secondActive;
    jointSnapshot.ConfigurationIndex = 4; jointSnapshot.ConfigurationTerm = 3;
    TConfigurationRecord stable{EConfigurationPhase::Stable, 5, 4, 3, firstActive, secondActive};

    {
        TDurableState state(name, 2);
        state.BindVoters(original);
        state.CurrentTerm = 4; state.Commit();
        state.InstallSnapshot(jointSnapshot);
        assert_true(state.GetBoundVoters() == original);
        assert_true(*state.GetSnapshot() == jointSnapshot);
    }

    TSnapshot stableSnapshot{6, 5, secondActive, "stable image"};
    stableSnapshot.StableConfiguration = true;
    stableSnapshot.ConfigurationIndex = 6; stableSnapshot.ConfigurationTerm = 5;
    TConfigurationRecord nextJoint{EConfigurationPhase::Joint, 6, 6, 5, secondActive, thirdActive};
    {
        TDurableState state(name, 2);
        assert_true(state.GetBoundVoters() == original);
        assert_true(*state.GetSnapshot() == jointSnapshot);
        auto history = TConfigurationHistory::FromJointSnapshot(*state.GetSnapshot());
        state.CurrentTerm = 5; state.Commit();
        state.Append(EncodeConfigurationEntry(stable));
        state.CommitIndex = state.LastLogIndex; state.Commit();
        auto scan = ScanCommittedConfigurationHistory(state, history);
        assert_false(scan.Committed.IsJoint());
        assert_true(scan.Committed.OldVoters() == secondActive);

        state.InstallSnapshot(stableSnapshot);
        state.CurrentTerm = 6; state.Commit();
        state.Append(EncodeConfigurationEntry(nextJoint));
        state.CommitIndex = state.LastLogIndex; state.Commit();
    }

    {
        auto state = std::make_shared<TDurableState>(name, 2);
        assert_true(state->GetBoundVoters() == original);
        assert_true(*state->GetSnapshot() == stableSnapshot);
        auto baseline = TConfigurationHistory::FromStableSnapshot(*state->GetSnapshot());
        const auto scan = ScanCommittedConfigurationHistory(*state, baseline);
        assert_true(scan.Committed.IsJoint());
        assert_true(scan.Committed.OldVoters() == secondActive);
        assert_true(scan.Committed.NewVoters() == thirdActive);
        assert_false(scan.HasUncommittedConfiguration());

        const auto fields = static_cast<TStateFields>(*state);
        Reject([&] { TRaft disabled(state, 2, {{4, std::make_shared<TEndpoint>()},
            {5, std::make_shared<TEndpoint>()}}); });
        assert_true(static_cast<TStateFields>(*state) == fields);
        assert_true(state->GetBoundVoters() == original);
    }
    std::error_code ignored; std::filesystem::remove_all(dir, ignored);
}
#endif
}
int main() {
    const CMUnitTest tests[] = {
        cmocka_unit_test(test_configuration_snapshot_projection_and_history_resume),
        cmocka_unit_test(test_history_validates_joint_stable_chain_and_independent_rollback),
        cmocka_unit_test(test_configuration_scan_compares_full_configuration_state),
        cmocka_unit_test(test_history_from_joint_snapshot_requires_a_consistent_identity),
        cmocka_unit_test(test_history_rejects_skips_wrong_predecessors_and_term_regression),
        cmocka_unit_test(test_scan_requires_each_configuration_predecessor_to_be_committed),
        cmocka_unit_test(test_scan_separates_commit_and_replaced_suffix),
        cmocka_unit_test(test_scan_from_joint_snapshot_and_rejects_mismatched_baselines),
        cmocka_unit_test(test_scan_from_stable_configuration_snapshot),
        cmocka_unit_test(test_scan_fails_closed_on_invalid_suffix),
        cmocka_unit_test(test_configuration_roundtrips_both_phases_and_bounded_maximum),
        cmocka_unit_test(test_configuration_rejects_invalid_fields_and_valid_crc_corruption),
        cmocka_unit_test(test_configuration_rpc_and_proposal_are_rejected_before_mutation),
        cmocka_unit_test(test_configuration_application_advances_without_calling_state_machine),
#ifdef __linux__
        cmocka_unit_test(test_durable_restart_rejects_reserved_entry_without_rewriting),
        cmocka_unit_test(test_legacy_restart_uses_safe_flag_lookup_and_preserves_files),
        cmocka_unit_test(test_offline_history_scan_after_durable_snapshot_and_restart),
        cmocka_unit_test(test_raft_recovers_joint_quorum_for_candidate_election),
        cmocka_unit_test(test_raft_keeps_joint_quorum_until_stable_successor_commits),
        cmocka_unit_test(test_raft_rejects_stable_snapshot_that_changes_active_voters),
        cmocka_unit_test(test_persisted_commit_boundary_drives_committed_configuration_scan),
        cmocka_unit_test(test_configuration_snapshots_preserve_original_binding_across_transitions),
#endif
    };
    return cmocka_run_group_tests(tests, nullptr, nullptr);
}
