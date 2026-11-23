#include <miniraft/raft.h>
#include <miniraft/persist.h>
#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <set>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
extern "C" {
#include <cmocka.h>
}

namespace {
namespace fs = std::filesystem;

struct TEndpoint : INode {
    std::function<void(TMessageHolder<TMessage>)> Deliver;
    explicit TEndpoint(std::function<void(TMessageHolder<TMessage>)> f) : Deliver(std::move(f)) {}
    void Send(TMessageHolder<TMessage> m) override { Deliver(std::move(m)); }
    void Drain() override {}
};

// Fails a test that reads an entry the snapshot has dropped.
struct TCheckedState : TState {
    TMessageHolder<TLogEntry> Get(int64_t index) const override {
        assert_true(uint64_t(index) > SnapshotIndex && uint64_t(index) <= LastLogIndex);
        return TState::Get(index);
    }
};

TMessageHolder<TLogEntry> LogValue(const std::string& value) {
    auto entry = NewHoldedMessage<TLogEntry>(sizeof(TLogEntry) + value.size());
    memcpy(entry->Data, value.data(), value.size());
    return entry;
}

std::string Value(const TMessageHolder<TMessage>& record) {
    auto entry = record.Mes;
    return std::string(reinterpret_cast<const TLogEntry*>(entry)->Data, entry->Len - sizeof(TLogEntry));
}

// Applied values; the image is one record per value.
struct TListRsm : IRsm {
    std::vector<std::string> Values;
    int Restores = 0;
    TMessageHolder<TMessage> Read(TMessageHolder<TCommandRequest>, uint64_t index) override {
        return NewHoldedMessage(TCommandResponse{.Index = index});
    }
    TMessageHolder<TLogEntry> Prepare(TMessageHolder<TCommandRequest> command) override {
        return LogValue(std::string(command->Data, command->Len - sizeof(TCommandRequest)));
    }
    TMessageHolder<TMessage> Write(TMessageHolder<TLogEntry> entry, uint64_t index) override {
        Values.push_back(Value(entry));
        LastAppliedIndex = index;
        return {};
    }
    TMessageHolder<TInstallSnapshotRequest> Snapshot() const override {
        auto image = NewHoldedMessage(TMessageEx{}, TInstallSnapshotRequest{.LastIncludedIndex = LastAppliedIndex});
        image.InitPayload(Values.size());
        for (size_t i = 0; i < Values.size(); ++i) {
            image.Payload[i] = LogValue(Values[i]);
        }
        return image;
    }
    void Apply(TMessageHolder<TInstallSnapshotRequest> snapshot) override {
        std::vector<std::string> values;
        for (uint32_t i = 0; i < snapshot.PayloadSize; ++i) {
            values.push_back(Value(snapshot.Payload[i]));
        }
        Values = std::move(values);
        ++Restores;
    }
};

// An application that does not support snapshots.
struct TPlainRsm : IRsm {
    std::vector<std::string> Values;
    mutable int SnapshotCalls = 0;
    TMessageHolder<TInstallSnapshotRequest> Snapshot() const override {
        ++SnapshotCalls;
        throw std::runtime_error("Not implemented");
    }
    void Apply(TMessageHolder<TInstallSnapshotRequest>) override {
        ++SnapshotCalls;
        throw std::runtime_error("Not implemented");
    }
    TMessageHolder<TMessage> Read(TMessageHolder<TCommandRequest>, uint64_t index) override {
        return NewHoldedMessage(TCommandResponse{.Index = index});
    }
    TMessageHolder<TLogEntry> Prepare(TMessageHolder<TCommandRequest> command) override {
        return LogValue(std::string(command->Data, command->Len - sizeof(TCommandRequest)));
    }
    TMessageHolder<TMessage> Write(TMessageHolder<TLogEntry> entry, uint64_t index) override {
        Values.push_back(Value(entry));
        LastAppliedIndex = index;
        return {};
    }
};

struct TFiles {
    fs::path Directory;
    std::string Name;
    TFiles() {
        static unsigned counter = 0;
        Directory = fs::temp_directory_path() /
            ("miniraft-snapshots-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(counter++));
        assert_true(fs::create_directory(Directory));
        Name = (Directory / "raft").string();
    }
    ~TFiles() { std::error_code ignored; fs::remove_all(Directory, ignored); }
};

// Virtual time and a manually scheduled network, one application per node.
struct TCluster {
    struct TNode {
        std::shared_ptr<IState> State;
        std::shared_ptr<TRaft> Raft;
        std::shared_ptr<TListRsm> Rsm;
        std::shared_ptr<TRequestProcessor> Processor;
    };
    ITimeSource::Time Now{};
    std::vector<TNode> Nodes;
    std::deque<TMessageHolder<TMessage>> Packets;
    std::set<uint32_t> Isolated;
    // Returns true to lose a packet.
    std::function<bool(const TMessageHolder<TMessage>&)> Drop;

    explicit TCluster(uint32_t count) : Nodes(count) {
        for (uint32_t id = 1; id <= count; ++id) {
            Start(id, std::make_shared<TCheckedState>());
        }
    }
    void Start(uint32_t id, std::shared_ptr<IState> state, std::shared_ptr<TListRsm> rsm = {}) {
        TNodeDict peers;
        for (uint32_t dst = 1; dst <= Nodes.size(); ++dst) {
            if (dst != id) peers[dst] = std::make_shared<TEndpoint>([this](auto m) {
                Packets.push_back(std::move(m));
            });
        }
        auto& node = Nodes.at(id - 1);
        node.State = std::move(state);
        node.Raft = std::make_shared<TRaft>(node.State, id, peers);
        node.Rsm = rsm ? rsm : std::make_shared<TListRsm>();
        node.Processor = std::make_shared<TRequestProcessor>(node.Raft, node.Rsm, TNodeDict{});
    }
    TNode& operator[](uint32_t id) { return Nodes.at(id - 1); }
    void Drain() {
        size_t budget = 100000;
        while (!Packets.empty()) {
            assert_true(budget-- > 0);
            auto m = Packets.front();
            Packets.pop_front();
            auto ex = m.Cast<TMessageEx>();
            if (Isolated.count(ex->Src) || Isolated.count(ex->Dst) || (Drop && Drop(m))) continue;
            Nodes.at(ex->Dst - 1).Raft->Process(Now, m);
        }
    }
    void Step(std::chrono::milliseconds dt = std::chrono::milliseconds(500)) {
        Now += dt;
        for (auto& node : Nodes) node.Raft->ProcessTimeout(Now);
        Drain();
        for (auto& node : Nodes) node.Processor->ProcessCommitted();
    }
    void Elect(uint32_t id) {
        Now += std::chrono::seconds(10);
        auto& raft = (*this)[id].Raft;
        raft->ProcessTimeout(Now);
        Drain();
        raft->ProcessTimeout(Now);
        Drain();
        assert_int_equal(raft->CurrentStateName(), EState::LEADER);
    }
    void Replicate(int rounds = 12) {
        for (int round = 0; round < rounds; ++round) Step();
    }
    void Write(uint32_t leader, const std::string& value) {
        (*this)[leader].Raft->Append(LogValue(value));
    }
    void Compact(uint32_t id) {
        auto& node = (*this)[id];
        node.Raft->Compact(node.Rsm->Snapshot());
    }
};

std::vector<std::string> Values(std::initializer_list<int> numbers) {
    std::vector<std::string> values;
    for (auto n : numbers) values.push_back(std::to_string(n));
    return values;
}

void test_compaction_drops_the_applied_prefix(void**) {
    TCluster cluster(3);
    cluster.Elect(1);
    for (int i = 1; i <= 4; ++i) cluster.Write(1, std::to_string(i));
    cluster.Replicate();
    auto& leader = cluster[1];
    const auto applied = leader.Rsm->LastAppliedIndex;
    const auto term = leader.State->LogTerm(applied);
    const auto last = leader.State->LastLogIndex;
    cluster.Compact(1);
    assert_int_equal(leader.State->SnapshotIndex, applied);
    assert_int_equal(leader.State->SnapshotTerm, term);
    assert_int_equal(leader.State->LogTerm(applied), term);
    assert_int_equal(leader.State->LastLogIndex, last);
    assert_int_equal(std::static_pointer_cast<TState>(leader.State)->Log.size(), last - applied);
    assert_int_equal(leader.State->GetSnapshot()->LastIncludedIndex, applied);
    assert_int_equal(leader.State->GetSnapshot().PayloadSize, 4);
    // An image that does not advance the snapshot changes nothing.
    cluster.Compact(1);
    assert_int_equal(leader.State->SnapshotIndex, applied);

    // Followers compact too; replication and a leader change go on above the snapshots.
    cluster.Compact(2);
    cluster.Compact(3);
    for (int i = 5; i <= 6; ++i) cluster.Write(1, std::to_string(i));
    cluster.Replicate();
    cluster.Elect(2);
    cluster.Write(2, "7");
    cluster.Replicate();
    for (uint32_t id = 1; id <= 3; ++id) {
        assert_true(cluster[id].Rsm->Values == Values({1, 2, 3, 4, 5, 6, 7}));
        assert_int_equal(cluster[id].State->SnapshotIndex, applied);
    }
}

void test_compaction_needs_a_committed_image(void**) {
    TCluster cluster(3);
    cluster.Elect(1);
    cluster.Write(1, "1");
    cluster.Replicate();
    cluster.Isolated = {2, 3};
    cluster.Write(1, "uncommitted");
    auto& leader = cluster[1];
    auto image = leader.Rsm->Snapshot();
    image->LastIncludedIndex = leader.State->LastLogIndex;
    bool rejected = false;
    try { leader.Raft->Compact(image); } catch (const std::invalid_argument&) { rejected = true; }
    assert_true(rejected);

    image = leader.Rsm->Snapshot();
    image.Payload[0] = NewHoldedMessage(TCommandResponse{});
    rejected = false;
    try { leader.Raft->Compact(image); } catch (const TMessageError&) { rejected = true; }
    assert_true(rejected);
    assert_int_equal(leader.State->SnapshotIndex, 0);
    assert_false(leader.State->GetSnapshot());
}

void test_append_entries_below_the_snapshot_match_its_prefix(void**) {
    TCluster cluster(3);
    cluster.Elect(1);
    for (int i = 1; i <= 3; ++i) cluster.Write(1, std::to_string(i));
    cluster.Replicate();
    auto& follower = cluster[2];
    cluster.Compact(2);
    const auto snapshot = follower.State->SnapshotIndex;
    assert_true(snapshot > 2);
    // A leader that probes below the follower's snapshot resends entries it has dropped.
    auto& leader = cluster[1];
    auto request = NewHoldedMessage(
        TMessageEx{.Src = 1, .Dst = 2, .Term = leader.State->CurrentTerm, .Seqno = 1000},
        TAppendEntriesRequest{.PrevLogIndex = 1, .PrevLogTerm = leader.State->LogTerm(1),
            .LeaderCommit = snapshot + 1, .LeaderId = 1, .Nentries = uint32_t(snapshot)});
    request.InitPayload(snapshot);
    for (uint64_t i = 0; i + 1 < snapshot; ++i) request.Payload[i] = leader.State->Get(i + 2);
    auto value = LogValue("4");
    value->Term = leader.State->CurrentTerm;
    request.Payload[snapshot - 1] = value;
    std::vector<TMessageHolder<TMessage>> replies;
    TNodeDict peers{{1, std::make_shared<TEndpoint>([&](auto m) { replies.push_back(m); })},
                    {3, std::make_shared<TEndpoint>([](auto) {})}};
    TRaft raft(follower.State, 2, peers);
    raft.Process(cluster.Now, request);
    assert_int_equal(replies.size(), 1);
    auto reply = replies[0].Cast<TAppendEntriesResponse>();
    assert_true(reply->Success);
    assert_int_equal(reply->MatchIndex, snapshot + 1);
    assert_int_equal(follower.State->LastLogIndex, snapshot + 1);
    assert_int_equal(follower.State->SnapshotIndex, snapshot);
    assert_int_equal(raft.GetVolatileState()->CommitIndex, snapshot + 1);
}

void test_restart_restores_the_application_from_snapshot_and_tail(void**) {
    TFiles files;
    TCluster cluster(1);
    cluster.Start(1, std::make_shared<TDiskState>(files.Name, 1));
    cluster.Elect(1);
    for (int i = 1; i <= 3; ++i) cluster.Write(1, std::to_string(i));
    cluster.Replicate(2);
    cluster.Compact(1);
    const auto snapshot = cluster[1].State->SnapshotIndex;
    assert_true(snapshot >= 4);
    // The prefix is gone from disk: only files of the snapshot remain.
    assert_false(fs::exists(files.Name + ".entries.1"));
    assert_false(fs::exists(files.Name + ".index.1"));
    assert_true(fs::exists(files.Name + ".snapshot.1." + std::to_string(snapshot)));
    cluster.Write(1, "4");
    cluster.Replicate(2);
    auto last = cluster[1].State->LastLogIndex;

    // A volatile application rebuilds from the image and the retained tail.
    for (int restart = 0; restart < 2; ++restart) {
        cluster.Start(1, std::make_shared<TDiskState>(files.Name, 1));
        auto& node = cluster[1];
        assert_int_equal(node.State->SnapshotIndex, snapshot);
        assert_int_equal(node.State->LastLogIndex, last);
        assert_int_equal(node.Raft->GetVolatileState()->CommitIndex, snapshot);
        node.Processor->ProcessCommitted();
        assert_int_equal(node.Rsm->Restores, 1);
        assert_int_equal(node.Rsm->LastAppliedIndex, snapshot);
        assert_true(node.Rsm->Values == Values({1, 2, 3}));
        cluster.Elect(1);
        cluster.Replicate(2);
        assert_true(node.Rsm->Values == Values({1, 2, 3, 4}));
        last = node.State->LastLogIndex;
    }
    // A durable application that is ahead of the snapshot keeps its progress.
    auto durable = std::make_shared<TListRsm>();
    durable->Values = Values({1, 2, 3, 4});
    durable->LastAppliedIndex = cluster[1].Rsm->LastAppliedIndex;
    cluster.Start(1, std::make_shared<TDiskState>(files.Name, 1), durable);
    cluster[1].Processor->ProcessCommitted();
    assert_int_equal(durable->Restores, 0);
    assert_true(durable->Values == Values({1, 2, 3, 4}));
}

void test_applications_without_snapshots_work_as_before(void**) {
    TCluster cluster(3);
    std::vector<std::shared_ptr<TPlainRsm>> applications;
    for (uint32_t id = 1; id <= 3; ++id) {
        auto rsm = std::make_shared<TPlainRsm>();
        applications.push_back(rsm);
        cluster[id].Processor = std::make_shared<TRequestProcessor>(cluster[id].Raft, rsm, TNodeDict{});
    }
    cluster.Elect(1);
    for (int i = 1; i <= 3; ++i) cluster.Write(1, std::to_string(i));
    cluster.Replicate();
    // Without compaction the library never asks for an image: a restart replays the log.
    auto restarted = std::make_shared<TPlainRsm>();
    TRequestProcessor processor(cluster[2].Raft, restarted, {});
    processor.ProcessCommitted();
    applications.push_back(restarted);
    for (const auto& rsm : applications) {
        assert_true(rsm->Values == Values({1, 2, 3}));
        assert_int_equal(rsm->SnapshotCalls, 0);
    }
    for (uint32_t id = 1; id <= 3; ++id) assert_int_equal(cluster[id].State->SnapshotIndex, 0);

    // With an image in storage, the application's refusal reaches the embedder and
    // the image does not count as applied.
    auto state = std::make_shared<TState>();
    state->InstallSnapshot(NewHoldedMessage(TMessageEx{}, TInstallSnapshotRequest{
        .LastIncludedIndex = 3, .LastIncludedTerm = 1, .Done = 1}));
    auto unsupported = std::make_shared<TPlainRsm>();
    TRequestProcessor restore(std::make_shared<TRaft>(state, 1, TNodeDict{}), unsupported, {});
    for (int attempt = 1; attempt <= 2; ++attempt) {
        bool reported = false;
        try { restore.ProcessCommitted(); } catch (const std::runtime_error&) { reported = true; }
        assert_true(reported);
        assert_int_equal(unsupported->SnapshotCalls, attempt);
        assert_int_equal(unsupported->LastAppliedIndex, 0);
    }
}

void test_storage_without_snapshots_rejects_compaction(void**) {
    struct TOldState : IState {
        std::vector<TMessageHolder<TLogEntry>> Log;
        void RemoveLast() override { Log.pop_back(); LastLogIndex = Log.size(); }
        void Append(TMessageHolder<TLogEntry> entry) override {
            Log.push_back(entry);
            LastLogIndex = Log.size();
            LastLogTerm = entry->Term;
        }
        TMessageHolder<TLogEntry> Get(int64_t index) const override { return Log[index - 1]; }
        void Commit() override {}
        TMessageHolder<TInstallSnapshotRequest> GetSnapshot() const override { return {}; }
        void InstallSnapshot(TMessageHolder<TInstallSnapshotRequest>) override {
            throw std::runtime_error("Not implemented");
        }
    };
    TCluster cluster(1);
    cluster.Start(1, std::make_shared<TOldState>());
    cluster.Elect(1);
    cluster.Write(1, "1");
    cluster.Replicate(2);
    bool unsupported = false;
    try { cluster.Compact(1); } catch (const std::runtime_error&) { unsupported = true; }
    assert_true(unsupported);
    assert_int_equal(cluster[1].State->SnapshotIndex, 0);
    assert_int_equal(cluster[1].State->LastLogIndex, 2);
}

} // namespace

int main() {
    const CMUnitTest tests[] = {
        cmocka_unit_test(test_compaction_drops_the_applied_prefix),
        cmocka_unit_test(test_compaction_needs_a_committed_image),
        cmocka_unit_test(test_append_entries_below_the_snapshot_match_its_prefix),
        cmocka_unit_test(test_restart_restores_the_application_from_snapshot_and_tail),
        cmocka_unit_test(test_applications_without_snapshots_work_as_before),
        cmocka_unit_test(test_storage_without_snapshots_rejects_compaction),
    };
    return cmocka_run_group_tests(tests, nullptr, nullptr);
}
