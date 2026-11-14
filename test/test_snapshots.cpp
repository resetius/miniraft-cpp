#include <miniraft/snapshot.h>
#include <miniraft/snapshot_codec.h>
#include <miniraft/raft.h>
#include <miniraft/durable.h>
#include "../examples/kv.h"
#include "../examples/kv_messages.h"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <iterator>
#ifdef __linux__
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
extern "C" {
#include <cmocka.h>
}

namespace {
template<class F> void Reject(F action) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    assert_true(rejected);
}
struct TSink : INode { void Send(TMessageHolder<TMessage>) override {} void Drain() override {} };
TMessageHolder<TWriteKv> Command(const std::string& key, const std::string& value) {
    auto command = NewHoldedMessage<TWriteKv>(sizeof(TWriteKv) + key.size() + value.size());
    command->Flags = TCommandRequest::EWrite;
    command->KeySize = key.size();
    command->ValSize = value.size();
    memcpy(command->TKvEntry::Data, key.data(), key.size());
    memcpy(command->TKvEntry::Data + key.size(), value.data(), value.size());
    return command;
}
std::string Read(TKv& kv, const std::string& key) {
    auto command = NewHoldedMessage<TReadKv>(sizeof(TReadKv) + key.size());
    command->KeySize = key.size();
    memcpy(command->Data, key.data(), key.size());
    auto reply = kv.Read(command, kv.LastAppliedIndex).Cast<TCommandResponse>();
    return std::string(reply->Data, reply->Len - sizeof(TCommandResponse));
}
void Apply(TKv& kv, const std::string& key, const std::string& value, uint64_t index) {
    kv.Write(kv.Prepare(Command(key, value)), index);
}
TSnapshot Sample() { return {2, 1, {1, 2, 3}, std::string("a\0b", 3)}; }
TSnapshot JointSample() {
    auto snapshot = Sample();
    snapshot.JointVoters = {3, 4, 5};
    snapshot.ConfigurationIndex = 1; snapshot.ConfigurationTerm = 1;
    return snapshot;
}
void FixChecksum(std::string& bytes) {
    bytes.resize(bytes.size() - 4);
    NSnapshotCodec::Put(bytes, NSnapshotCodec::Checksum(bytes), 4);
}
struct TFiles {
    std::filesystem::path Directory;
    std::string Name;
    TFiles() {
        static unsigned next = 0;
        Directory = std::filesystem::temp_directory_path() / ("miniraft-snapshot-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(next++));
        assert_true(std::filesystem::create_directory(Directory));
        Name = (Directory / "raft").string();
    }
    ~TFiles() { std::error_code ignored; std::filesystem::remove_all(Directory, ignored); }
    std::string Image() const { return Name + ".snapshot.1"; }
};
void test_snapshot_codec_roundtrip_and_all_byte_corruption(void**) {
    const auto original = Sample();
    const auto encoded = EncodeSnapshot(original);
    assert_true(DecodeSnapshot(encoded) == original);
    for (size_t i = 0; i < encoded.size(); ++i) {
        auto damaged = encoded;
        damaged[i] ^= 1;
        Reject([&] { DecodeSnapshot(damaged); });
        Reject([&] { DecodeSnapshot(std::string_view(encoded).substr(0, i)); });
    }
}
void test_joint_snapshot_codec_roundtrip_and_corruption(void**) {
    auto fixed = EncodeSnapshot(Sample());
    // Golden v1 image, including the original CRC and embedded NUL byte.
    const std::string hex = "4d5246545330303102000000000000000100000000000000030000000300000000000000010000000200000003000000610062e0c541c6";
    std::string golden;
    for (size_t i = 0; i < hex.size(); i += 2) golden.push_back(char(std::stoul(hex.substr(i, 2), nullptr, 16)));
    assert_true(fixed == golden);
    assert_true(DecodeSnapshot(golden) == Sample());
    assert_false(DecodeSnapshot(fixed).IsJoint());
    const auto original = JointSample();
    const auto encoded = EncodeSnapshot(original);
    assert_true(encoded.substr(0, 8) == "MRFTS002");
    assert_true(DecodeSnapshot(encoded) == original);
    for (size_t i = 0; i < encoded.size(); ++i) {
        auto damaged = encoded; damaged[i] ^= 1;
        Reject([&] { DecodeSnapshot(damaged); });
        Reject([&] { DecodeSnapshot(std::string_view(encoded).substr(0, i)); });
    }
}
void test_stable_configuration_snapshot_codec_and_runtime_guards(void**) {
    auto snapshot = Sample(); snapshot.StableConfiguration = true;
    snapshot.ConfigurationIndex = 1; snapshot.ConfigurationTerm = 1;
    const auto encoded = EncodeSnapshot(snapshot);
    assert_true(encoded.substr(0, 8) == "MRFTS003");
    assert_true(DecodeSnapshot(encoded) == snapshot); assert_false(snapshot.IsJoint());
    for (size_t i = 0; i < encoded.size(); ++i) {
        auto damaged = encoded; damaged[i] ^= 1;
        Reject([&] { DecodeSnapshot(damaged); });
        Reject([&] { DecodeSnapshot(std::string_view(encoded).substr(0, i)); });
    }
    for (int mode = 0; mode < 6; ++mode) {
        auto bad = snapshot;
        if (mode == 0) bad.ConfigurationIndex = 0;
        if (mode == 1) bad.ConfigurationIndex = 3;
        if (mode == 2) bad.ConfigurationTerm = 0;
        if (mode == 3) bad.ConfigurationTerm = 2;
        if (mode == 4) { bad.Term = 2; bad.ConfigurationIndex = 2; bad.ConfigurationTerm = 1; }
        if (mode == 5) bad.JointVoters = {3, 4};
        Reject([&] { EncodeSnapshot(bad); });
    }
    for (size_t offset : {size_t(36), size_t(44)}) {
        auto damaged = encoded;
        for (size_t i = offset; i < offset + 8; ++i) damaged[i] = char(0xff);
        FixChecksum(damaged); Reject([&] { DecodeSnapshot(damaged); });
    }
    auto memory = std::make_shared<TState>();
    Reject([&] { memory->InstallSnapshot(snapshot); });
    memory->Snapshot = std::make_shared<const TSnapshot>(snapshot);
    Reject([&] { TRaft unsupported(memory, 1, {{2, std::make_shared<TSink>()}, {3, std::make_shared<TSink>()}}); });
    assert_true(memory->GetBoundVoters().empty());
    auto bound = std::make_shared<TState>();
    bound->BindVoters(snapshot.Voters);
    bound->InstallSnapshot(snapshot);
    TRaft recovered(bound, 1, {{2, std::make_shared<TSink>()}, {3, std::make_shared<TSink>()}});
    assert_int_equal(recovered.GetVolatileState()->CommitIndex, snapshot.Index);
    auto changedBinding = std::make_shared<TState>();
    changedBinding->BindVoters({1, 2, 4});
    changedBinding->CurrentTerm = snapshot.Term;
    changedBinding->Snapshot = std::make_shared<const TSnapshot>(snapshot);
    changedBinding->SnapshotIndex = changedBinding->LastLogIndex = snapshot.Index;
    changedBinding->SnapshotTerm = changedBinding->LastLogTerm = snapshot.Term;
    Reject([&] { TRaft invalid(changedBinding, 1, {{2, std::make_shared<TSink>()}, {3, std::make_shared<TSink>()}}); });
    assert_true(changedBinding->GetBoundVoters() == std::vector<uint32_t>({1, 2, 4}));
#ifdef __linux__
    TFiles files;
    { TSnapshotStore store(files.Name, 1); store.Save(snapshot); assert_true(store.Load().value() == snapshot); }
    { TDurableState state(files.Name, 1); Reject([&] { state.InstallSnapshot(snapshot); }); }
#endif
}
void test_joint_snapshot_metadata_rejects_invalid_boundaries_and_voters(void**) {
    for (int mode = 0; mode < 10; ++mode) {
        auto bad = JointSample();
        if (mode == 0) bad.JointVoters.clear();
        if (mode == 1) bad.JointVoters = {0, 1};
        if (mode == 2) bad.JointVoters = {1, 1};
        if (mode == 3) bad.JointVoters = {2, 1};
        if (mode == 4) bad.JointVoters.resize(TSnapshot::MaxVoters + 1, 1);
        if (mode == 5) bad.ConfigurationIndex = 0;
        if (mode == 6) bad.ConfigurationIndex = bad.Index + 1;
        if (mode == 7) bad.ConfigurationTerm = 0;
        if (mode == 8) bad.ConfigurationTerm = bad.Term + 1;
        if (mode == 9) { bad.Term = 2; bad.ConfigurationIndex = bad.Index; bad.ConfigurationTerm = 1; }
        Reject([&] { EncodeSnapshot(bad); });
    }
    for (size_t offset : {size_t(36), size_t(44), size_t(52)}) {
        auto damaged = EncodeSnapshot(JointSample());
        const auto width = offset == 52 ? 4 : 8;
        for (size_t i = offset; i < offset + width; ++i) damaged[i] = char(0xff);
        FixChecksum(damaged); Reject([&] { DecodeSnapshot(damaged); });
    }
    auto empty = EncodeSnapshot(JointSample());
    for (size_t i = 36; i < 56; ++i) empty[i] = 0;
    FixChecksum(empty); Reject([&] { DecodeSnapshot(empty); });
    auto mismatch = EncodeSnapshot(JointSample());
    mismatch[16] = 2; // snapshot term
    mismatch[36] = 2; // configuration index equals the snapshot index
    FixChecksum(mismatch); Reject([&] { DecodeSnapshot(mismatch); });
}
void test_joint_snapshot_cannot_be_installed_or_restored_by_fixed_runtime(void**) {
    auto snapshot = JointSample();
    auto state = std::make_shared<TState>();
    state->Append(NewHoldedMessage<TLogEntry>()); state->Append(NewHoldedMessage<TLogEntry>());
    const auto before = static_cast<TStateFields>(*state);
    Reject([&] { state->InstallSnapshot(snapshot); });
    assert_true(static_cast<TStateFields>(*state) == before);
    TRaft raft(state, 1, {{2, std::make_shared<TSink>()}, {3, std::make_shared<TSink>()}});
    TKv app;
    Reject([&] { RestoreSnapshot(snapshot, raft, app); });
    assert_int_equal(app.LastAppliedIndex, 0);
    auto recovered = std::make_shared<TState>();
    recovered->Snapshot = std::make_shared<const TSnapshot>(snapshot);
    Reject([&] { TRaft unsupported(recovered, 1, {{2, std::make_shared<TSink>()}, {3, std::make_shared<TSink>()}}); });
    assert_true(recovered->GetBoundVoters().empty());
}
void test_startup_rejects_inconsistent_snapshot_boundaries_before_binding(void**) {
    for (int mode = 0; mode < 11; ++mode) {
        auto state = std::make_shared<TState>(); state->CurrentTerm = 2;
        state->SnapshotIndex = state->LastLogIndex = 5;
        state->SnapshotTerm = state->LastLogTerm = 2;
        TSnapshot image{5, 2, {1, 2}, "opaque"};
        if (mode == 1) image.Index = 4;
        if (mode == 2) image.Term = 1;
        if (mode == 3) state->LastLogTerm = 1;
        if (mode == 4) state->CurrentTerm = 1;
        if (mode == 5) image.Voters = {1, 1};
        if (mode == 6 || mode == 7) {
            state->SnapshotIndex = state->LastLogIndex = 0;
            state->SnapshotTerm = mode == 6 ? 2 : 0; state->LastLogTerm = 0;
        }
        if (mode == 8) image.Voters = {1, 3};
        if (mode != 0 && mode != 6) state->Snapshot = std::make_shared<TSnapshot>(image);
        if (mode == 9 || mode == 10) {
            auto entry = NewHoldedMessage<TLogEntry>(); entry->Term = mode == 9 ? 1 : 3; state->Append(entry);
        }
        const auto before = static_cast<TStateFields>(*state);
        Reject([&] { TRaft invalid(state, 1, {{2, std::make_shared<TSink>()}}); });
        assert_true(static_cast<TStateFields>(*state) == before);
        assert_true(state->GetBoundVoters().empty());
    }
    auto valid = std::make_shared<TState>(); valid->CurrentTerm = 2;
    valid->SnapshotIndex = valid->LastLogIndex = 5; valid->SnapshotTerm = valid->LastLogTerm = 2;
    valid->Snapshot = std::make_shared<TSnapshot>(TSnapshot{5, 2, {1, 2}, "opaque"});
    TRaft raft(valid, 1, {{2, std::make_shared<TSink>()}});
    assert_int_equal(raft.GetVolatileState()->CommitIndex, 5);
    assert_true(valid->GetBoundVoters() == std::vector<uint32_t>({1, 2}));
    auto entry = NewHoldedMessage<TLogEntry>(); entry->Term = 2; valid->Append(entry);
    TRaft withSuffix(valid, 1, {{2, std::make_shared<TSink>()}});
    assert_int_equal(withSuffix.GetVolatileState()->CommitIndex, 5);
}
void test_startup_rejects_invalid_compacted_suffix_terms(void**) {
    for (int mode = 0; mode < 4; ++mode) {
        auto state = std::make_shared<TState>(); state->CurrentTerm = 3;
        state->SnapshotIndex = state->LastLogIndex = 5; state->SnapshotTerm = state->LastLogTerm = 2;
        state->Snapshot = std::make_shared<TSnapshot>(TSnapshot{5, 2, {1, 2}, "opaque"});
        auto first = NewHoldedMessage<TLogEntry>(); first->Term = mode == 0 ? 1 : mode == 2 ? 0 : mode == 3 ? 4 : 2;
        auto last = NewHoldedMessage<TLogEntry>(); last->Term = mode == 1 ? 3 : 2;
        state->Append(first); state->Append(last);
        if (mode == 1) state->LastLogTerm = 2; // in range but differs from the actual last entry
        const auto before = static_cast<TStateFields>(*state);
        Reject([&] { TRaft invalid(state, 1, {{2, std::make_shared<TSink>()}}); });
        assert_true(static_cast<TStateFields>(*state) == before); assert_true(state->GetBoundVoters().empty());
    }
    auto valid = std::make_shared<TState>(); valid->CurrentTerm = 3;
    valid->SnapshotIndex = valid->LastLogIndex = 5; valid->SnapshotTerm = valid->LastLogTerm = 2;
    valid->Snapshot = std::make_shared<TSnapshot>(TSnapshot{5, 2, {1, 2}, "opaque"});
    for (uint64_t term : {2, 2, 3}) { auto entry = NewHoldedMessage<TLogEntry>(); entry->Term = term; valid->Append(entry); }
    TRaft raft(valid, 1, {{2, std::make_shared<TSink>()}});
    assert_int_equal(raft.GetVolatileState()->CommitIndex, 5);
}
void test_snapshot_metadata_and_declared_lengths_are_bounded(void**) {
    auto snapshot = Sample();
    for (uint64_t index : {uint64_t(0), UINT64_MAX}) {
        auto bad = snapshot; bad.Index = index;
        Reject([&] { EncodeSnapshot(bad); });
    }
    for (auto voters : {std::vector<uint32_t>{}, std::vector<uint32_t>{0, 1},
         std::vector<uint32_t>{1, 1}, std::vector<uint32_t>{2, 1}}) {
        auto bad = snapshot; bad.Voters = voters;
        Reject([&] { EncodeSnapshot(bad); });
    }
    auto bad = snapshot; bad.Term = 0;
    Reject([&] { EncodeSnapshot(bad); });
    for (size_t offset : {size_t(0), size_t(8), size_t(16), size_t(24), size_t(28)}) {
        auto damaged = EncodeSnapshot(snapshot);
        for (size_t i = offset; i < offset + (offset == 24 ? 4 : 8); ++i) damaged[i] = char(0xff);
        FixChecksum(damaged); // Exercise semantic validation, not just CRC rejection.
        if (offset == 16) {
            // Terms are full-width; UINT64_MAX is valid in the codec.
            assert_true(DecodeSnapshot(damaged).Term == UINT64_MAX);
        } else Reject([&] { DecodeSnapshot(damaged); });
    }
}
void test_kv_snapshot_is_canonical_and_binary_safe(void**) {
    TKv a, b;
    const std::string binary("a\0z", 3);
    Apply(a, "z", "last", 1); Apply(a, "a", binary, 2); Apply(a, "gone", "remove", 3); Apply(a, "gone", {}, 4);
    Apply(b, "a", binary, 1); Apply(b, "z", "last", 2);
    assert_true(a.SaveSnapshot() == b.SaveSnapshot());
    TKv restored;
    restored.LoadSnapshot(a.SaveSnapshot(), 4);
    assert_int_equal(restored.LastAppliedIndex, 4);
    assert_true(Read(restored, "a") == binary);
    assert_true(Read(restored, "z") == "last");
    assert_true(Read(restored, "gone").empty());
}
void test_kv_invalid_snapshot_preserves_existing_state_and_progress(void**) {
    TKv source, target;
    Apply(source, "a", "one", 1); Apply(source, "b", "two", 2);
    Apply(target, "old", "preserved", 1);
    const auto bytes = source.SaveSnapshot();
    for (size_t size = 0; size < bytes.size(); ++size) {
        Reject([&] { target.LoadSnapshot(std::string_view(bytes).substr(0, size), 2); });
        assert_true(Read(target, "old") == "preserved");
        assert_int_equal(target.LastAppliedIndex, 1);
    }
    auto trailing = bytes + "x";
    Reject([&] { target.LoadSnapshot(trailing, 2); });
    auto duplicate = bytes;
    duplicate[36] = 'a'; // second record's key; duplicate canonical key must fail
    Reject([&] { target.LoadSnapshot(duplicate, 2); });
    std::string oversized = "MRKV0001";
    NSnapshotCodec::Put(oversized, 1, 8);
    NSnapshotCodec::Put(oversized, 1, 4);
    NSnapshotCodec::Put(oversized, uint64_t(UINT16_MAX) + 1, 4);
    NSnapshotCodec::Append(oversized, "a");
    NSnapshotCodec::Append(oversized, std::string(size_t(UINT16_MAX) + 1, 'x'));
    Reject([&] { target.LoadSnapshot(oversized, 2); });
    assert_true(Read(target, "old") == "preserved");
    assert_int_equal(target.LastAppliedIndex, 1);
}
void test_capture_requires_committed_applied_progress(void**) {
    auto state = std::make_shared<TState>();
    TRaft raft(state, 1, {});
    TKv kv;
    Reject([&] { CaptureSnapshot(raft, kv); });
    auto entry = kv.Prepare(Command("a", "one"));
    state->Append(entry);
    kv.Write(entry, 1);
    Reject([&] { CaptureSnapshot(raft, kv); });
    const_cast<TVolatileState*>(raft.GetVolatileState())->CommitIndex = 1;
    const auto snapshot = CaptureSnapshot(raft, kv);
    assert_int_equal(snapshot.Index, 1);
    assert_int_equal(snapshot.Term, 1);
    assert_true(snapshot.Voters == std::vector<uint32_t>{1});
}
void test_restore_rejects_mismatched_log_configuration_and_used_application(void**) {
    auto state = std::make_shared<TState>();
    TKv source;
    auto entry = source.Prepare(Command("a", "one"));
    state->Append(entry); source.Write(entry, 1);
    TRaft raft(state, 1, {{2, std::make_shared<TSink>()}, {3, std::make_shared<TSink>()}});
    const_cast<TVolatileState*>(raft.GetVolatileState())->CommitIndex = 1;
    auto snapshot = CaptureSnapshot(raft, source);
    for (int mode = 0; mode < 4; ++mode) {
        auto bad = snapshot;
        if (mode == 0) bad.Index = 2;
        if (mode == 1) bad.Term = 2;
        if (mode == 2) bad.Voters = {1, 2};
        if (mode == 3) bad.Data = "invalid KV data";
        TKv restored;
        Reject([&] { RestoreSnapshot(bad, raft, restored); });
        assert_int_equal(restored.LastAppliedIndex, 0);
        assert_true(Read(restored, "a").empty());
    }
    TKv restored;
    RestoreSnapshot(snapshot, raft, restored);
    Reject([&] { RestoreSnapshot(snapshot, raft, restored); });
    assert_true(Read(restored, "a") == "one");
}
void test_startup_restores_stale_application_from_durable_snapshot_boundary(void**) {
    auto state = std::make_shared<TState>();
    TKv source; Apply(source, "snapshot", "committed", 2);
    TSnapshot snapshot{2, 1, {1}, source.SaveSnapshot()};
    state->InstallSnapshot(snapshot);
    auto raft = std::make_shared<TRaft>(state, 1, TNodeDict{});

    auto stale = std::make_shared<TKv>(); Apply(*stale, "stale", "must be replaced", 1);
    assert_int_equal(stale->LastAppliedIndex, 1);
    TRequestProcessor processor(raft, stale, {});
    assert_int_equal(stale->LastAppliedIndex, 2);
    assert_true(Read(*stale, "snapshot") == "committed");
    assert_true(Read(*stale, "stale").empty());
    assert_int_equal(raft->GetVolatileState()->CommitIndex, 2);
}
#ifdef __linux__
void test_stable_configuration_snapshot_durable_runtime_recovery_and_compaction(void**) {
    TFiles files;
    TKv source; Apply(source, "a", "one", 1); Apply(source, "b", "two", 2);
    TSnapshot snapshot{2, 1, {1, 2, 3}, source.SaveSnapshot()};
    snapshot.StableConfiguration = true;
    snapshot.ConfigurationIndex = 1; snapshot.ConfigurationTerm = 1;
    {
        TDurableState state(files.Name, 1);
        state.BindVoters(snapshot.Voters);
        state.InstallSnapshot(snapshot);
    }
    TSnapshot compacted;
    {
        auto state = std::make_shared<TDurableState>(files.Name, 1);
        assert_true(state->GetBoundVoters() == snapshot.Voters);
        assert_true(*state->GetSnapshot() == snapshot);
        auto raft = std::make_shared<TRaft>(state, 1,
            TNodeDict{{2, std::make_shared<TSink>()}, {3, std::make_shared<TSink>()}});
        auto app = std::make_shared<TKv>();
        TRequestProcessor processor(raft, app, {});
        assert_int_equal(app->LastAppliedIndex, 2);
        assert_true(Read(*app, "a") == "one"); assert_true(Read(*app, "b") == "two");

        auto entry = app->Prepare(Command("tail", "after recovery")); entry->Term = 1;
        state->Append(entry); app->Write(entry, 3);
        const_cast<TVolatileState*>(raft->GetVolatileState())->CommitIndex = 3;
        compacted = raft->Compact();
        assert_true(compacted.StableConfiguration);
        assert_int_equal(compacted.ConfigurationIndex, snapshot.ConfigurationIndex);
        assert_int_equal(compacted.ConfigurationTerm, snapshot.ConfigurationTerm);
        assert_int_equal(compacted.Index, 3);
    }
    auto state = std::make_shared<TDurableState>(files.Name, 1);
    assert_true(*state->GetSnapshot() == compacted);
    auto raft = std::make_shared<TRaft>(state, 1,
        TNodeDict{{2, std::make_shared<TSink>()}, {3, std::make_shared<TSink>()}});
    auto app = std::make_shared<TKv>(); TRequestProcessor processor(raft, app, {});
    assert_int_equal(app->LastAppliedIndex, compacted.Index);
    assert_true(Read(*app, "a") == "one"); assert_true(Read(*app, "b") == "two");
    assert_true(Read(*app, "tail") == "after recovery");
}
void test_unbound_durable_configuration_snapshot_recovery_fails_closed(void**) {
    TFiles files; const auto path = files.Name + ".durable.1";
    {
        TDurableState state(files.Name, 1);
        state.InstallSnapshot(Sample());
    }
    std::ifstream input(path, std::ios::binary);
    std::string bytes(std::istreambuf_iterator<char>(input), {}); input.close();
    assert_true(bytes.substr(0, 8) == "MRFTD002");
    auto snapshot = Sample(); snapshot.StableConfiguration = true;
    snapshot.ConfigurationIndex = 1; snapshot.ConfigurationTerm = 1;
    const auto image = EncodeSnapshot(snapshot);
    constexpr size_t imageLengthOffset = 40;
    constexpr size_t imageOffset = 48;
    uint64_t oldLength = 0;
    for (int i = 0; i < 8; ++i) oldLength |= uint64_t(uint8_t(bytes[imageLengthOffset + i])) << (8 * i);
    assert_true(oldLength == bytes.size() - imageOffset);
    bytes.replace(imageOffset, size_t(oldLength), image);
    for (int i = 0; i < 8; ++i) bytes[imageLengthOffset + i] = char(uint64_t(image.size()) >> (8 * i));
    for (int i = 0; i < 8; ++i) bytes[8 + i] = char(uint64_t(bytes.size() - 20) >> (8 * i));
    const auto crc = NSnapshotCodec::Checksum(bytes.substr(0, 8) + bytes.substr(20));
    for (int i = 0; i < 4; ++i) bytes[16 + i] = char(crc >> (8 * i));
    { std::ofstream output(path, std::ios::binary | std::ios::trunc); output.write(bytes.data(), bytes.size()); }
    Reject([&] { TDurableState state(files.Name, 1); });
    std::ifstream after(path, std::ios::binary);
    assert_true(std::string(std::istreambuf_iterator<char>(after), {}) == bytes);
}
void test_joint_snapshot_export_and_durable_roundtrip_but_runtime_rejects(void**) {
    TFiles files;
    { TSnapshotStore exportStore(files.Name, 1); exportStore.Save(JointSample());
      assert_true(exportStore.Load().value() == JointSample()); }
    const auto path = files.Name + ".durable.1";
    {
        TDurableState state(files.Name, 1);
        state.BindVoters(JointSample().Voters);
        state.InstallSnapshot(JointSample());
    }
    std::string bytes;
    {
        TDurableState state(files.Name, 1);
        assert_true(state.GetBoundVoters() == JointSample().Voters);
        assert_true(*state.GetSnapshot() == JointSample());
        const auto before = static_cast<TStateFields>(state);
        auto mismatched = JointSample(); mismatched.Voters = {1, 2, 4};
        std::ifstream input(path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(input), {});
        Reject([&] { state.InstallSnapshot(mismatched); });
        assert_true(static_cast<TStateFields>(state) == before);
        std::ifstream unchanged(path, std::ios::binary);
        assert_true(std::string(std::istreambuf_iterator<char>(unchanged), {}) == bytes);
    }
    Reject([&] { TRaft raft(std::make_shared<TDurableState>(files.Name, 1), 1,
                           {{2, std::make_shared<TSink>()}, {3, std::make_shared<TSink>()}}); });
    std::ifstream after(path, std::ios::binary);
    assert_true(std::string(std::istreambuf_iterator<char>(after), {}) == bytes);
}
void test_snapshot_store_rejects_oversized_files_and_temp_symlinks(void**) {
    TFiles files;
    TSnapshotStore store(files.Name, 1);
    store.Save(Sample());
    const auto saved = store.Load();
    std::filesystem::create_symlink(files.Image(), files.Image() + ".tmp");
    Reject([&] { store.Save(Sample()); });
    assert_true(store.Load() == saved);
    std::filesystem::resize_file(files.Image(), TSnapshot::MaxBytes + 1);
    Reject([&] { store.Load(); });
    assert_true(std::filesystem::file_size(files.Image()) == TSnapshot::MaxBytes + 1);
}
void test_snapshot_store_roundtrip_lock_and_corrupt_recovery(void**) {
    TFiles files;
    {
        TSnapshotStore store(files.Name, 1);
        assert_false(store.Load().has_value());
        Reject([&] { TSnapshotStore second(files.Name, 1); });
        store.Save(Sample());
        assert_true(store.Load() == std::optional<TSnapshot>{Sample()});
    }
    std::ifstream input(files.Image(), std::ios::binary);
    std::string bytes(std::istreambuf_iterator<char>(input), {});
    input.close();
    bytes.back() ^= 1;
    { std::ofstream out(files.Image(), std::ios::binary); out.write(bytes.data(), bytes.size()); }
    TSnapshotStore store(files.Name, 1);
    Reject([&] { store.Load(); });
    std::ifstream after(files.Image(), std::ios::binary);
    assert_true(std::string(std::istreambuf_iterator<char>(after), {}) == bytes);
}
void test_snapshot_crash_at_each_publication_boundary(void**) {
    using TStage = TSnapshotStore::EStage;
    for (bool initial : {false, true}) for (auto stage : {TStage::TempCreated, TStage::Written,
         TStage::FileSynced, TStage::Renamed, TStage::DirectorySynced}) {
        TFiles files;
        if (!initial) { TSnapshotStore store(files.Name, 1); store.Save(Sample()); }
        auto updated = Sample(); updated.Index = 3; updated.Data = "new committed state";
        const auto child = fork();
        assert_true(child >= 0);
        if (child == 0) {
            try {
                TSnapshotStore store(files.Name, 1, [&](auto point) { if (point == stage) _exit(90); });
                store.Save(updated);
            } catch (...) { _exit(91); }
            _exit(92);
        }
        int status = 0;
        assert_int_equal(waitpid(child, &status, 0), child);
        assert_true(WIFEXITED(status)); assert_int_equal(WEXITSTATUS(status), 90);
        TSnapshotStore store(files.Name, 1);
        auto selected = store.Load();
        if (stage >= TStage::Renamed) assert_true(selected == std::optional<TSnapshot>{updated});
        else if (initial) assert_false(selected.has_value());
        else assert_true(selected == std::optional<TSnapshot>{Sample()});
    }
}
void test_snapshot_publication_error_preserves_old_checkpoint(void**) {
    TFiles files;
    { TSnapshotStore store(files.Name, 1); store.Save(Sample()); }
    TSnapshotStore store(files.Name, 1, [](auto stage) {
        if (stage == TSnapshotStore::EStage::Written) throw std::runtime_error("injected checkpoint failure");
    });
    auto updated = Sample(); updated.Index = 3;
    Reject([&] { store.Save(updated); });
    assert_true(store.Load() == std::optional<TSnapshot>{Sample()});
}
void test_durable_kv_restart_restores_checkpoint_and_replays_only_tail(void**) {
    struct TCountingKv : TKv {
        size_t Applied = 0;
        TMessageHolder<TMessage> Write(TMessageHolder<TLogEntry> entry, uint64_t index) override {
            ++Applied; return TKv::Write(entry, index);
        }
    };
    TFiles files;
    {
        auto state = std::make_shared<TDurableState>(files.Name, 1);
        auto raft = std::make_shared<TRaft>(state, 1, TNodeDict{});
        auto kv = std::make_shared<TKv>();
        TRequestProcessor processor(raft, kv, {});
        auto now = ITimeSource::Time{};
        raft->ProcessTimeout(now);
        auto write = [&](const std::string& key, const std::string& value) {
            processor.OnCommandRequest(Command(key, value), {});
            now += std::chrono::milliseconds(500);
            raft->ProcessTimeout(now); processor.ProcessCommitted();
        };
        write("a", "one");
        { TSnapshotStore store(files.Name, 1); store.Save(CaptureSnapshot(*raft, *kv)); }
        write("b", "tail");
        assert_int_equal(state->LastLogIndex, 3);
    }
    auto state = std::make_shared<TDurableState>(files.Name, 1);
    auto raft = std::make_shared<TRaft>(state, 1, TNodeDict{});
    auto kv = std::make_shared<TCountingKv>();
    TSnapshotStore store(files.Name, 1);
    auto snapshot = store.Load(); assert_true(snapshot.has_value());
    RestoreSnapshot(*snapshot, *raft, *kv);
    assert_true(Read(*kv, "a") == "one"); assert_true(Read(*kv, "b").empty());
    TRequestProcessor processor(raft, kv, {});
    processor.ProcessCommitted(); assert_int_equal(kv->Applied, 0);
    raft->ProcessTimeout(ITimeSource::Time{}); processor.ProcessCommitted();
    assert_int_equal(kv->Applied, 1);
    assert_true(Read(*kv, "a") == "one"); assert_true(Read(*kv, "b") == "tail");
    assert_int_equal(kv->LastAppliedIndex, 4);
    assert_int_equal(state->LastLogIndex, 4); // no compaction: every entry is retained
}
#endif
}
int main() {
    const CMUnitTest tests[] = {
        cmocka_unit_test(test_snapshot_codec_roundtrip_and_all_byte_corruption),
        cmocka_unit_test(test_joint_snapshot_codec_roundtrip_and_corruption),
        cmocka_unit_test(test_stable_configuration_snapshot_codec_and_runtime_guards),
        cmocka_unit_test(test_joint_snapshot_metadata_rejects_invalid_boundaries_and_voters),
        cmocka_unit_test(test_joint_snapshot_cannot_be_installed_or_restored_by_fixed_runtime),
        cmocka_unit_test(test_startup_rejects_inconsistent_snapshot_boundaries_before_binding),
        cmocka_unit_test(test_startup_rejects_invalid_compacted_suffix_terms),
        cmocka_unit_test(test_snapshot_metadata_and_declared_lengths_are_bounded),
        cmocka_unit_test(test_kv_snapshot_is_canonical_and_binary_safe),
        cmocka_unit_test(test_kv_invalid_snapshot_preserves_existing_state_and_progress),
        cmocka_unit_test(test_capture_requires_committed_applied_progress),
        cmocka_unit_test(test_restore_rejects_mismatched_log_configuration_and_used_application),
        cmocka_unit_test(test_startup_restores_stale_application_from_durable_snapshot_boundary),
#ifdef __linux__
        cmocka_unit_test(test_stable_configuration_snapshot_durable_runtime_recovery_and_compaction),
        cmocka_unit_test(test_unbound_durable_configuration_snapshot_recovery_fails_closed),
        cmocka_unit_test(test_joint_snapshot_export_and_durable_roundtrip_but_runtime_rejects),
        cmocka_unit_test(test_snapshot_store_rejects_oversized_files_and_temp_symlinks),
        cmocka_unit_test(test_snapshot_store_roundtrip_lock_and_corrupt_recovery),
        cmocka_unit_test(test_snapshot_crash_at_each_publication_boundary),
        cmocka_unit_test(test_snapshot_publication_error_preserves_old_checkpoint),
        cmocka_unit_test(test_durable_kv_restart_restores_checkpoint_and_replays_only_tail),
#endif
    };
    return cmocka_run_group_tests(tests, nullptr, nullptr);
}
