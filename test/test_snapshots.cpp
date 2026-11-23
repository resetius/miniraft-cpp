#include <miniraft/raft.h>
#include <miniraft/persist.h>
#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <set>
#include <gtest/gtest.h>

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
        bool live = uint64_t(index) > SnapshotIndex && uint64_t(index) <= LastLogIndex;
        EXPECT_TRUE(live) << "read of dropped entry " << index;
        if (!live) {
            throw std::out_of_range("entry was dropped by the snapshot");
        }
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

// Fails once, before taking the image or after it, like a failed metadata write.
struct TFailingState : TCheckedState {
    bool AfterInstall = false;
    bool LogicError = false;
    bool Failed = false;
    void Fail(const char* what) {
        Failed = true;
        if (LogicError) {
            throw std::invalid_argument(what);
        }
        throw std::runtime_error(what);
    }
    void InstallSnapshot(TMessageHolder<TInstallSnapshotRequest> snapshot) override {
        if (!AfterInstall && !Failed) {
            Fail("No space left");
        }
        TState::InstallSnapshot(snapshot);
        if (!Failed) {
            Fail("Cannot write Raft metadata");
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
        EXPECT_TRUE(fs::create_directory(Directory));
        if (!fs::is_directory(Directory)) {
            throw std::runtime_error("cannot create " + Directory.string());
        }
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
            ASSERT_TRUE(budget-- > 0);
            auto m = Packets.front();
            Packets.pop_front();
            auto ex = m.Cast<TMessageEx>();
            if (Isolated.count(ex->Src) || Isolated.count(ex->Dst) || (Drop && Drop(m))) continue;
            Nodes.at(ex->Dst - 1).Raft->Process(Now, m);
        }
    }
    // Only leaders run their timers unless all = true; elections happen in Elect.
    void Step(std::chrono::milliseconds dt = std::chrono::milliseconds(500), bool all = false) {
        Now += dt;
        for (auto& node : Nodes) {
            if (all || node.Raft->CurrentStateName() == EState::LEADER) {
                node.Raft->ProcessTimeout(Now);
            }
        }
        ASSERT_NO_FATAL_FAILURE(Drain());
        for (auto& node : Nodes) node.Processor->ProcessCommitted();
    }
    void Elect(uint32_t id) {
        Now += std::chrono::seconds(10);
        auto& raft = (*this)[id].Raft;
        raft->ProcessTimeout(Now);
        ASSERT_NO_FATAL_FAILURE(Drain());
        raft->ProcessTimeout(Now);
        ASSERT_NO_FATAL_FAILURE(Drain());
        ASSERT_EQ(raft->CurrentStateName(), EState::LEADER);
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
        ASSERT_NO_FATAL_FAILURE(Elect(1));
        Isolated = {3};
        for (int i = 1; i <= writes; ++i) Write(1, std::to_string(i));
        Replicate();
        Compact(1);
        Isolated.clear();
    }
    void AssertCaughtUp(uint32_t leader, uint32_t follower) {
        auto& l = (*this)[leader];
        auto& f = (*this)[follower];
        ASSERT_EQ(f.State->LastLogIndex, l.State->LastLogIndex);
        ASSERT_EQ(f.State->LastLogTerm, l.State->LastLogTerm);
        ASSERT_EQ(l.Raft->GetVolatileState()->MatchIndex.at(follower), l.State->LastLogIndex);
        ASSERT_TRUE(f.Rsm->Values == l.Rsm->Values);
        ASSERT_EQ(f.Rsm->LastAppliedIndex, l.Rsm->LastAppliedIndex);
        ASSERT_TRUE(l.Raft->GetVolatileState()->SnapshotTransfers.empty());
    }
    template<class F> void StepUntil(F done) {
        for (int i = 0; i < 100 && !done(); ++i) Step();
        ASSERT_TRUE(done());
    }
};

std::vector<std::string> Values(std::initializer_list<int> numbers) {
    std::vector<std::string> values;
    for (auto n : numbers) values.push_back(std::to_string(n));
    return values;
}

TEST(Snapshots, compaction_drops_the_applied_prefix) {
    TCluster cluster(3);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    for (int i = 1; i <= 4; ++i) cluster.Write(1, std::to_string(i));
    cluster.Replicate();
    auto& leader = cluster[1];
    const auto applied = leader.Rsm->LastAppliedIndex;
    const auto term = leader.State->LogTerm(applied);
    const auto last = leader.State->LastLogIndex;
    cluster.Compact(1);
    ASSERT_EQ(leader.State->SnapshotIndex, applied);
    ASSERT_EQ(leader.State->SnapshotTerm, term);
    ASSERT_EQ(leader.State->LogTerm(applied), term);
    ASSERT_EQ(leader.State->LastLogIndex, last);
    ASSERT_EQ(std::static_pointer_cast<TState>(leader.State)->Log.size(), last - applied);
    ASSERT_EQ(leader.State->GetSnapshot()->LastIncludedIndex, applied);
    // The configuration and the four values.
    ASSERT_EQ(leader.State->GetSnapshot().PayloadSize, 5);
    // An image that does not advance the snapshot changes nothing.
    cluster.Compact(1);
    ASSERT_EQ(leader.State->SnapshotIndex, applied);

    // Followers compact too; replication and a leader change go on above the snapshots.
    cluster.Compact(2);
    cluster.Compact(3);
    for (int i = 5; i <= 6; ++i) cluster.Write(1, std::to_string(i));
    cluster.Replicate();
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(2));
    cluster.Write(2, "7");
    cluster.Replicate();
    for (uint32_t id = 1; id <= 3; ++id) {
        ASSERT_TRUE(cluster[id].Rsm->Values == Values({1, 2, 3, 4, 5, 6, 7}));
        ASSERT_EQ(cluster[id].State->SnapshotIndex, applied);
    }
}

TEST(Snapshots, compaction_needs_a_committed_image) {
    TCluster cluster(3);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    cluster.Write(1, "1");
    cluster.Replicate();
    cluster.Isolated = {2, 3};
    cluster.Write(1, "uncommitted");
    auto& leader = cluster[1];
    auto image = leader.Rsm->Snapshot();
    image->LastIncludedIndex = leader.State->LastLogIndex;
    bool rejected = false;
    try { leader.Raft->Compact(image); } catch (const std::invalid_argument&) { rejected = true; }
    ASSERT_TRUE(rejected);

    image = leader.Rsm->Snapshot();
    image.Payload[0] = NewHoldedMessage(TCommandResponse{});
    rejected = false;
    try { leader.Raft->Compact(image); } catch (const TMessageError&) { rejected = true; }
    ASSERT_TRUE(rejected);
    ASSERT_EQ(leader.State->SnapshotIndex, 0);
    ASSERT_FALSE(leader.State->GetSnapshot());
}

TEST(Snapshots, append_entries_below_the_snapshot_match_its_prefix) {
    TCluster cluster(3);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    for (int i = 1; i <= 3; ++i) cluster.Write(1, std::to_string(i));
    cluster.Replicate();
    auto& follower = cluster[2];
    cluster.Compact(2);
    const auto snapshot = follower.State->SnapshotIndex;
    ASSERT_TRUE(snapshot > 2);
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
    ASSERT_EQ(replies.size(), 1);
    auto reply = replies[0].Cast<TAppendEntriesResponse>();
    ASSERT_TRUE(reply->Success);
    ASSERT_EQ(reply->MatchIndex, snapshot + 1);
    ASSERT_EQ(follower.State->LastLogIndex, snapshot + 1);
    ASSERT_EQ(follower.State->SnapshotIndex, snapshot);
    ASSERT_EQ(raft.GetVolatileState()->CommitIndex, snapshot + 1);
}

TEST(Snapshots, restart_restores_the_application_from_snapshot_and_tail) {
    TFiles files;
    TCluster cluster(1);
    cluster.Start(1, std::make_shared<TDiskState>(files.Name, 1));
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    for (int i = 1; i <= 3; ++i) cluster.Write(1, std::to_string(i));
    cluster.Replicate(2);
    cluster.Compact(1);
    const auto snapshot = cluster[1].State->SnapshotIndex;
    ASSERT_TRUE(snapshot >= 4);
    // The prefix is gone from disk: only files of the snapshot remain.
    ASSERT_FALSE(fs::exists(files.Name + ".entries.1"));
    ASSERT_FALSE(fs::exists(files.Name + ".index.1"));
    ASSERT_TRUE(fs::exists(files.Name + ".snapshot.1." + std::to_string(snapshot)));
    cluster.Write(1, "4");
    cluster.Replicate(2);
    auto last = cluster[1].State->LastLogIndex;

    // A volatile application rebuilds from the image and the retained tail.
    for (int restart = 0; restart < 2; ++restart) {
        cluster.Start(1, std::make_shared<TDiskState>(files.Name, 1));
        auto& node = cluster[1];
        ASSERT_EQ(node.State->SnapshotIndex, snapshot);
        ASSERT_EQ(node.State->LastLogIndex, last);
        ASSERT_EQ(node.Raft->GetVolatileState()->CommitIndex, snapshot);
        node.Processor->ProcessCommitted();
        ASSERT_EQ(node.Rsm->Restores, 1);
        ASSERT_EQ(node.Rsm->LastAppliedIndex, snapshot);
        ASSERT_TRUE(node.Rsm->Values == Values({1, 2, 3}));
        ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
        cluster.Replicate(2);
        ASSERT_TRUE(node.Rsm->Values == Values({1, 2, 3, 4}));
        last = node.State->LastLogIndex;
    }
    // A durable application that is ahead of the snapshot keeps its progress.
    auto durable = std::make_shared<TListRsm>();
    durable->Values = Values({1, 2, 3, 4});
    durable->LastAppliedIndex = cluster[1].Rsm->LastAppliedIndex;
    cluster.Start(1, std::make_shared<TDiskState>(files.Name, 1), durable);
    cluster[1].Processor->ProcessCommitted();
    ASSERT_EQ(durable->Restores, 0);
    ASSERT_TRUE(durable->Values == Values({1, 2, 3, 4}));
}

TEST(Snapshots, applications_without_snapshots_work_as_before) {
    TCluster cluster(3);
    std::vector<std::shared_ptr<TPlainRsm>> applications;
    for (uint32_t id = 1; id <= 3; ++id) {
        auto rsm = std::make_shared<TPlainRsm>();
        applications.push_back(rsm);
        cluster[id].Processor = std::make_shared<TRequestProcessor>(cluster[id].Raft, rsm, TNodeDict{});
    }
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    for (int i = 1; i <= 3; ++i) cluster.Write(1, std::to_string(i));
    cluster.Replicate();
    // Without compaction the library never asks for an image: a restart replays the log.
    auto restarted = std::make_shared<TPlainRsm>();
    TRequestProcessor processor(cluster[2].Raft, restarted, {});
    processor.ProcessCommitted();
    applications.push_back(restarted);
    for (const auto& rsm : applications) {
        ASSERT_TRUE(rsm->Values == Values({1, 2, 3}));
        ASSERT_EQ(rsm->SnapshotCalls, 0);
    }
    for (uint32_t id = 1; id <= 3; ++id) {
        ASSERT_EQ(cluster[id].State->SnapshotIndex, 0);
    }

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
        ASSERT_TRUE(reported);
        ASSERT_EQ(unsupported->SnapshotCalls, attempt);
        ASSERT_EQ(unsupported->LastAppliedIndex, 0);
    }
}

TEST(Snapshots, storage_without_snapshots_rejects_compaction) {
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
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    cluster.Write(1, "1");
    cluster.Replicate(2);
    bool unsupported = false;
    try { cluster.Compact(1); } catch (const std::runtime_error&) { unsupported = true; }
    ASSERT_TRUE(unsupported);
    ASSERT_EQ(cluster[1].State->SnapshotIndex, 0);
    ASSERT_EQ(cluster[1].State->LastLogIndex, 2);
}

TEST(Snapshots, lagging_follower_installs_a_chunked_snapshot_despite_losses) {
    TCluster cluster(3);
    cluster.Lag(4);
    int chunks = 0, replies = 0;
    cluster.Drop = [&](const auto& m) {
        if (IsType(m, EMessageType::INSTALL_SNAPSHOT_REQUEST)) return ++chunks % 2 == 0;
        if (IsType(m, EMessageType::INSTALL_SNAPSHOT_RESPONSE)) return ++replies % 3 == 0;
        return false;
    };
    auto& follower = cluster[3];
    ASSERT_NO_FATAL_FAILURE(cluster.StepUntil([&] { return !follower.Raft->GetVolatileState()->IncomingRecords.empty(); }));
    // A partial image changes nothing; a restart loses it and the transfer starts over.
    ASSERT_EQ(follower.State->SnapshotIndex, 0);
    ASSERT_TRUE(follower.Rsm->Values.empty());
    cluster.Start(3, follower.State, follower.Rsm);
    cluster.Replicate(100);
    ASSERT_EQ(follower.State->SnapshotIndex, cluster[1].State->SnapshotIndex);
    ASSERT_TRUE(follower.Rsm->Values == Values({1, 2, 3, 4}));
    ASSERT_EQ(follower.Rsm->Restores, 1);
    ASSERT_NO_FATAL_FAILURE(cluster.AssertCaughtUp(1, 3));
}

TEST(Snapshots, delayed_reply_after_retries_advances_the_transfer) {
    TCluster cluster(3);
    cluster.Lag();
    std::vector<TMessageHolder<TMessage>> held;
    cluster.Drop = [&](const auto& m) {
        if (!IsType(m, EMessageType::INSTALL_SNAPSHOT_RESPONSE)) return false;
        held.push_back(m);
        return true;
    };
    ASSERT_NO_FATAL_FAILURE(cluster.StepUntil([&] { return held.size() >= 3; }));
    // Retries of a chunk keep its identity, so a late reply to the first one still counts.
    for (auto reply : held) {
        ASSERT_EQ(reply.Cast<TMessageEx>()->Seqno, held.front().Cast<TMessageEx>()->Seqno);
    }
    const auto& transfer = cluster[1].Raft->GetVolatileState()->SnapshotTransfers.at(3);
    ASSERT_EQ(transfer.Offset, 0);
    cluster.Drop = {};
    // A stale-term reply with the chunk's seqno is dropped: the transfer does not move.
    auto& leader = cluster[1];
    leader.Raft->Process(cluster.Now, NewHoldedMessage(
        TMessageEx{.Src = 3, .Dst = 1, .Term = leader.State->CurrentTerm - 1,
            .Seqno = held.front().Cast<TMessageEx>()->Seqno},
        TInstallSnapshotResponse{}));
    ASSERT_EQ(transfer.Offset, 0);
    ASSERT_EQ(leader.State->CurrentTerm, held.front().Cast<TMessageEx>()->Term);
    cluster.Packets.push_back(held[0]);
    ASSERT_NO_FATAL_FAILURE(cluster.Drain());
    ASSERT_EQ(transfer.Offset, TMessageLimits{}.MaxEntries);
    cluster.Packets.push_back(held[1]);
    ASSERT_NO_FATAL_FAILURE(cluster.Drain());
    ASSERT_EQ(transfer.Offset, TMessageLimits{}.MaxEntries);
    cluster.Replicate(40);
    ASSERT_NO_FATAL_FAILURE(cluster.AssertCaughtUp(1, 3));
}

TEST(Snapshots, leader_change_in_the_middle_of_a_transfer) {
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
    ASSERT_NO_FATAL_FAILURE(cluster.StepUntil([&] { return !follower.Raft->GetVolatileState()->IncomingRecords.empty(); }));
    // Keep the old leader's chunks to deliver them after the new leader takes over.
    cluster.Drop = [&](const auto& m) {
        if (!IsType(m, EMessageType::INSTALL_SNAPSHOT_REQUEST)) return false;
        late.push_back(m);
        return true;
    };
    cluster.Step();
    cluster.Step();
    cluster.Drop = {};
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(2));
    ASSERT_TRUE(cluster[1].Raft->GetVolatileState()->SnapshotTransfers.empty());
    for (auto& m : late) cluster.Packets.push_back(m);
    cluster.Write(2, "4");
    cluster.Replicate(40);
    ASSERT_EQ(follower.State->SnapshotIndex, cluster[2].State->SnapshotIndex);
    ASSERT_EQ(follower.Rsm->Restores, 1);
    ASSERT_TRUE(follower.Rsm->Values == Values({1, 2, 3, 4}));
    ASSERT_NO_FATAL_FAILURE(cluster.AssertCaughtUp(2, 3));
    ASSERT_NO_FATAL_FAILURE(cluster.AssertCaughtUp(2, 1));
}

// Any exception type is a failure, never a final refusal of the image.
TEST(Snapshots, storage_failure_reaches_the_embedder_and_is_retried) {
    for (auto [afterInstall, logicError] : {std::pair{false, false}, {true, false}, {false, true}, {true, true}}) {
        TCluster cluster(3);
        auto state = std::make_shared<TFailingState>();
        state->AfterInstall = afterInstall;
        state->LogicError = logicError;
        cluster.Start(3, state);
        cluster.Lag();
        int chunks = 0;
        cluster.Drop = [&](const auto& m) {
            chunks += IsType(m, EMessageType::INSTALL_SNAPSHOT_REQUEST);
            return false;
        };
        int reported = 0;
        for (int i = 0; i < 40; ++i) {
            try { cluster.Step(); } catch (const std::exception&) { ++reported; }
        }
        // Only the last chunk comes again: it installs the kept image or finds it installed.
        ASSERT_EQ(reported, 1);
        ASSERT_EQ(chunks, 4);
        ASSERT_EQ(cluster[3].State->SnapshotIndex, cluster[1].State->SnapshotIndex);
        ASSERT_EQ(cluster[3].Rsm->Restores, 1);
        ASSERT_NO_FATAL_FAILURE(cluster.AssertCaughtUp(1, 3));
    }
}

TEST(Snapshots, compaction_during_a_transfer_keeps_its_image) {
    TCluster cluster(3);
    cluster.Lag();
    auto& transfers = cluster[1].Raft->GetVolatileState()->SnapshotTransfers;
    ASSERT_NO_FATAL_FAILURE(cluster.StepUntil([&] { return transfers.count(3) && transfers.at(3).Offset; }));
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
    ASSERT_TRUE(installed.size() >= 2);
    ASSERT_EQ(installed.front(), first);
    cluster.Replicate(40);
    ASSERT_NO_FATAL_FAILURE(cluster.AssertCaughtUp(1, 3));
}

TEST(Snapshots, covered_and_contradicting_images) {
    TCluster cluster(3);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
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
    ASSERT_FALSE(send(3, term - 1, 0));
    ASSERT_FALSE(send(last + 5, term - 1, 0));
    // The log has the boundary entry: acknowledged at once, from any chunk.
    auto reply = send(3, term, 4096);
    ASSERT_TRUE(reply);
    ASSERT_EQ(reply->MatchIndex, 3);
    cluster.Compact(2);
    reply = send(2, term, 0);
    ASSERT_TRUE(reply);
    ASSERT_EQ(reply->MatchIndex, 2);
    ASSERT_EQ(follower.State->LastLogIndex, last);
    ASSERT_EQ(follower.State->SnapshotIndex, last);
    ASSERT_TRUE(follower.Raft->GetVolatileState()->IncomingRecords.empty());
}

} // namespace

