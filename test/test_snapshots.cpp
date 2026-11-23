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

// Applied values; the image is one record per value after ExtraRecords empty ones.
struct TListRsm : IRsm {
    std::vector<std::string> Values;
    uint32_t ExtraRecords = 0;
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
        image.InitPayload(ExtraRecords + Values.size());
        for (uint32_t i = 0; i < ExtraRecords; ++i) {
            image.Payload[i] = LogValue("");
        }
        for (size_t i = 0; i < Values.size(); ++i) {
            image.Payload[ExtraRecords + i] = LogValue(Values[i]);
        }
        return image;
    }
    void Apply(TMessageHolder<TInstallSnapshotRequest> snapshot) override {
        std::vector<std::string> values;
        for (uint32_t i = 0; i < snapshot.PayloadSize; ++i) {
            if (auto value = Value(snapshot.Payload[i]); !value.empty()) {
                values.push_back(value);
            }
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

struct TRefusingState : TCheckedState {
    void InstallSnapshot(TMessageHolder<TInstallSnapshotRequest>) override {
        throw std::logic_error("Snapshot refused");
    }
};

// Fails once, before taking the image or after it, like a failed metadata write.
struct TFailingState : TCheckedState {
    bool AfterInstall = false;
    bool Failed = false;
    void InstallSnapshot(TMessageHolder<TInstallSnapshotRequest> snapshot) override {
        if (!AfterInstall && !Failed) {
            Failed = true;
            throw std::runtime_error("No space left");
        }
        TState::InstallSnapshot(snapshot);
        if (!Failed) {
            Failed = true;
            throw std::runtime_error("Cannot write Raft metadata");
        }
    }
};

bool IsType(const TMessageHolder<TMessage>& message, EMessageType type) {
    return message->Type == uint32_t(type);
}

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
    // Only leaders run their timers; elections happen in Elect.
    void Step(std::chrono::milliseconds dt = std::chrono::milliseconds(500)) {
        Now += dt;
        for (auto& node : Nodes) {
            if (node.Raft->CurrentStateName() == EState::LEADER) node.Raft->ProcessTimeout(Now);
        }
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
    // Node 3 misses some writes, then node 1 compacts past them: images of three chunks.
    void Lag(int writes = 3) {
        for (auto& node : Nodes) node.Rsm->ExtraRecords = 9000;
        Elect(1);
        Isolated = {3};
        for (int i = 1; i <= writes; ++i) Write(1, std::to_string(i));
        Replicate();
        Compact(1);
        Isolated.clear();
    }
    void AssertCaughtUp(uint32_t leader, uint32_t follower) {
        auto& l = (*this)[leader];
        auto& f = (*this)[follower];
        assert_int_equal(f.State->LastLogIndex, l.State->LastLogIndex);
        assert_int_equal(f.State->LastLogTerm, l.State->LastLogTerm);
        assert_int_equal(l.Raft->GetVolatileState()->MatchIndex.at(follower), l.State->LastLogIndex);
        assert_true(f.Rsm->Values == l.Rsm->Values);
        assert_int_equal(f.Rsm->LastAppliedIndex, l.Rsm->LastAppliedIndex);
        assert_true(l.Raft->GetVolatileState()->SnapshotTransfers.empty());
    }
    template<class F> void StepUntil(F done) {
        for (int i = 0; i < 100 && !done(); ++i) Step();
        assert_true(done());
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

void test_lagging_follower_installs_a_chunked_snapshot_despite_losses(void**) {
    TCluster cluster(3);
    cluster.Lag(4);
    int chunks = 0, replies = 0;
    cluster.Drop = [&](const auto& m) {
        if (IsType(m, EMessageType::INSTALL_SNAPSHOT_REQUEST)) return ++chunks % 2 == 0;
        if (IsType(m, EMessageType::INSTALL_SNAPSHOT_RESPONSE)) return ++replies % 3 == 0;
        return false;
    };
    auto& follower = cluster[3];
    cluster.StepUntil([&] { return !follower.Raft->GetVolatileState()->IncomingRecords.empty(); });
    // A partial image changes nothing; a restart loses it and the transfer starts over.
    assert_int_equal(follower.State->SnapshotIndex, 0);
    assert_true(follower.Rsm->Values.empty());
    cluster.Start(3, follower.State, follower.Rsm);
    cluster.Replicate(100);
    assert_int_equal(follower.State->SnapshotIndex, cluster[1].State->SnapshotIndex);
    assert_true(follower.Rsm->Values == Values({1, 2, 3, 4}));
    assert_int_equal(follower.Rsm->Restores, 1);
    cluster.AssertCaughtUp(1, 3);
}

void test_delayed_reply_after_retries_advances_the_transfer(void**) {
    TCluster cluster(3);
    cluster.Lag();
    std::vector<TMessageHolder<TMessage>> held;
    cluster.Drop = [&](const auto& m) {
        if (!IsType(m, EMessageType::INSTALL_SNAPSHOT_RESPONSE)) return false;
        held.push_back(m);
        return true;
    };
    cluster.StepUntil([&] { return held.size() >= 3; });
    // Retries of a chunk keep its identity, so a late reply to the first one still counts.
    for (auto reply : held) {
        assert_int_equal(reply.Cast<TMessageEx>()->Seqno, held.front().Cast<TMessageEx>()->Seqno);
    }
    const auto& transfer = cluster[1].Raft->GetVolatileState()->SnapshotTransfers.at(3);
    assert_int_equal(transfer.Offset, 0);
    cluster.Drop = {};
    cluster.Packets.push_back(held[0]);
    cluster.Drain();
    assert_int_equal(transfer.Offset, TMessageLimits{}.MaxEntries);
    cluster.Packets.push_back(held[1]);
    cluster.Drain();
    assert_int_equal(transfer.Offset, TMessageLimits{}.MaxEntries);
    cluster.Replicate(40);
    cluster.AssertCaughtUp(1, 3);
}

void test_leader_change_in_the_middle_of_a_transfer(void**) {
    TCluster cluster(3);
    cluster.Lag();
    cluster.Compact(2);
    std::vector<TMessageHolder<TMessage>> late;
    cluster.Drop = [&](const auto& m) {
        if (!IsType(m, EMessageType::INSTALL_SNAPSHOT_RESPONSE)) return false;
        late.push_back(m);
        return true;
    };
    auto& follower = cluster[3];
    cluster.StepUntil([&] { return !follower.Raft->GetVolatileState()->IncomingRecords.empty(); });
    // Keep the old leader's chunks to deliver them after the new leader takes over.
    cluster.Drop = [&](const auto& m) {
        if (!IsType(m, EMessageType::INSTALL_SNAPSHOT_REQUEST)) return false;
        late.push_back(m);
        return true;
    };
    cluster.Step();
    cluster.Step();
    cluster.Drop = {};
    cluster.Elect(2);
    assert_true(cluster[1].Raft->GetVolatileState()->SnapshotTransfers.empty());
    for (auto& m : late) cluster.Packets.push_back(m);
    cluster.Write(2, "4");
    cluster.Replicate(40);
    assert_int_equal(follower.State->SnapshotIndex, cluster[2].State->SnapshotIndex);
    assert_int_equal(follower.Rsm->Restores, 1);
    assert_true(follower.Rsm->Values == Values({1, 2, 3, 4}));
    cluster.AssertCaughtUp(2, 3);
    cluster.AssertCaughtUp(2, 1);
}

void test_refused_image_is_not_sent_again(void**) {
    TCluster cluster(3);
    cluster.Start(3, std::make_shared<TRefusingState>());
    cluster.Lag();
    int chunks = 0;
    cluster.Drop = [&](const auto& m) {
        chunks += IsType(m, EMessageType::INSTALL_SNAPSHOT_REQUEST);
        return false;
    };
    cluster.Replicate(40);
    const auto sent = chunks;
    const auto term = cluster[1].State->CurrentTerm;
    assert_int_equal(sent, 3);
    // The follower only gets heartbeats: no new attempts, and its election never comes.
    cluster.Write(1, "4");
    cluster.Replicate(100);
    assert_int_equal(chunks, sent);
    assert_true(cluster[3].Raft->GetVolatileState()->ElectionDue > cluster.Now);
    assert_int_equal(cluster[1].Raft->CurrentStateName(), EState::LEADER);
    for (uint32_t id = 1; id <= 3; ++id) assert_int_equal(cluster[id].State->CurrentTerm, term);
    assert_int_equal(cluster[3].Raft->GetLeaderId(), 1);
    assert_int_equal(cluster[3].State->SnapshotIndex, 0);
    assert_true(cluster[3].Rsm->Values.empty());
    cluster.AssertCaughtUp(1, 2);
    // A newer image gets one more attempt.
    cluster.Compact(1);
    cluster.Replicate(40);
    assert_int_equal(chunks, 2 * sent);
}

void test_storage_failure_reaches_the_embedder_and_is_retried(void**) {
    for (bool afterInstall : {false, true}) {
        TCluster cluster(3);
        auto state = std::make_shared<TFailingState>();
        state->AfterInstall = afterInstall;
        cluster.Start(3, state);
        cluster.Lag();
        int chunks = 0;
        cluster.Drop = [&](const auto& m) {
            chunks += IsType(m, EMessageType::INSTALL_SNAPSHOT_REQUEST);
            return false;
        };
        int reported = 0;
        for (int i = 0; i < 40; ++i) {
            try { cluster.Step(); } catch (const std::runtime_error&) { ++reported; }
        }
        // Only the last chunk comes again: it installs the kept image or finds it installed.
        assert_int_equal(reported, 1);
        assert_int_equal(chunks, 4);
        assert_int_equal(cluster[3].State->SnapshotIndex, cluster[1].State->SnapshotIndex);
        assert_int_equal(cluster[3].Rsm->Restores, 1);
        cluster.AssertCaughtUp(1, 3);
    }
}

void test_compaction_during_a_transfer_keeps_its_image(void**) {
    TCluster cluster(3);
    cluster.Lag();
    auto& transfers = cluster[1].Raft->GetVolatileState()->SnapshotTransfers;
    cluster.StepUntil([&] { return transfers.count(3) && transfers.at(3).Offset; });
    const auto first = transfers.at(3).Snapshot->LastIncludedIndex;
    std::vector<uint64_t> installed;
    for (int step = 0; step < 20; ++step) {
        cluster.Write(1, std::to_string(100 + step));
        cluster.Step();
        cluster.Compact(1);
        const auto index = cluster[3].State->SnapshotIndex;
        if (index && (installed.empty() || installed.back() != index)) installed.push_back(index);
    }
    // The first transfer ends with the image it started with, newer ones follow.
    assert_true(installed.size() >= 2);
    assert_int_equal(installed.front(), first);
    cluster.Replicate(40);
    cluster.AssertCaughtUp(1, 3);
}

void test_covered_and_contradicting_images(void**) {
    TCluster cluster(3);
    cluster.Elect(1);
    for (int i = 1; i <= 3; ++i) cluster.Write(1, std::to_string(i));
    cluster.Replicate();
    auto& follower = cluster[2];
    const auto term = cluster[1].State->CurrentTerm;
    const auto last = follower.State->LastLogIndex;
    auto send = [&](uint64_t index, uint64_t snapshotTerm, uint64_t offset) {
        cluster.Packets.clear();
        follower.Raft->Process(cluster.Now, NewHoldedMessage(
            TMessageEx{.Src = 1, .Dst = 2, .Term = term, .Seqno = 777},
            TInstallSnapshotRequest{.LastIncludedIndex = index, .LastIncludedTerm = snapshotTerm,
                .Offset = offset, .LeaderId = 1}));
        return cluster.Packets.empty() ? TMessageHolder<TInstallSnapshotResponse>{}
            : cluster.Packets.back().Cast<TInstallSnapshotResponse>();
    };
    // An image against the committed prefix is dropped like any invalid RPC.
    assert_false(send(3, term - 1, 0));
    assert_false(send(last + 5, term - 1, 0));
    // The log has the boundary entry: acknowledged at once, from any chunk.
    auto reply = send(3, term, 4096);
    assert_true(reply && reply->Success);
    assert_int_equal(reply->MatchIndex, 3);
    cluster.Compact(2);
    reply = send(2, term, 0);
    assert_true(reply && reply->Success);
    assert_int_equal(reply->MatchIndex, 2);
    assert_int_equal(follower.State->LastLogIndex, last);
    assert_int_equal(follower.State->SnapshotIndex, last);
    assert_true(follower.Raft->GetVolatileState()->IncomingRecords.empty());
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
        cmocka_unit_test(test_lagging_follower_installs_a_chunked_snapshot_despite_losses),
        cmocka_unit_test(test_delayed_reply_after_retries_advances_the_transfer),
        cmocka_unit_test(test_leader_change_in_the_middle_of_a_transfer),
        cmocka_unit_test(test_refused_image_is_not_sent_again),
        cmocka_unit_test(test_storage_failure_reaches_the_embedder_and_is_retried),
        cmocka_unit_test(test_compaction_during_a_transfer_keeps_its_image),
        cmocka_unit_test(test_covered_and_contradicting_images),
    };
    return cmocka_run_group_tests(tests, nullptr, nullptr);
}
