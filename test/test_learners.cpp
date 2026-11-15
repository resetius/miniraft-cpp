#include <miniraft/raft.h>
#include <miniraft/durable.h>
#include <miniraft/persist.h>
#include <miniraft/snapshot_codec.h>
#include "../examples/kv.h"
#include "../examples/kv_messages.h"
#include <deque>
#include <functional>
#include <set>
#include <filesystem>
#include <fstream>
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
struct TEndpoint : INode {
    std::function<void(TMessageHolder<TMessage>)> Callback;
    explicit TEndpoint(std::function<void(TMessageHolder<TMessage>)> callback = {}) : Callback(std::move(callback)) {}
    void Send(TMessageHolder<TMessage> message) override { if (Callback) Callback(std::move(message)); }
    void Drain() override {}
};
TNodeDict Peers(uint32_t self, uint32_t count = 4) {
    TNodeDict peers;
    for (uint32_t id = 1; id <= count; ++id) if (id != self) peers[id] = std::make_shared<TEndpoint>();
    return peers;
}
TMessageHolder<TWriteKv> Command(const std::string& key, const std::string& value) {
    auto command = NewHoldedMessage<TWriteKv>(sizeof(TWriteKv) + key.size() + value.size());
    command->Flags = TCommandRequest::EWrite; command->KeySize = key.size(); command->ValSize = value.size();
    memcpy(command->TKvEntry::Data, key.data(), key.size());
    memcpy(command->TKvEntry::Data + key.size(), value.data(), value.size());
    return command;
}
std::string Read(TKv& kv, const std::string& key) {
    auto command = NewHoldedMessage<TReadKv>(sizeof(TReadKv) + key.size());
    command->KeySize = key.size(); memcpy(command->Data, key.data(), key.size());
    auto result = kv.Read(command, kv.LastAppliedIndex).Cast<TCommandResponse>();
    return std::string(result->Data, result->Len - sizeof(TCommandResponse));
}
struct TApp : TKv {
    size_t Loads = 0;
    void LoadSnapshot(std::string_view bytes, uint64_t index) override { TKv::LoadSnapshot(bytes, index); ++Loads; }
};
struct TCluster {
    ITimeSource::Time Now{};
    std::vector<std::shared_ptr<TRaft>> Nodes{4};
    std::vector<std::shared_ptr<TApp>> Apps{4};
    std::vector<std::unique_ptr<TRequestProcessor>> Processors{4};
    std::deque<TMessageHolder<TMessage>> Packets;
    std::vector<uint32_t> VoteTargets;
    std::set<uint32_t> Isolated;
    bool StageVoterLearner = false;
    explicit TCluster(bool stageVoterLearner = false) : StageVoterLearner(stageVoterLearner) {
        for (uint32_t id = 1; id <= 4; ++id) Build(id, std::make_shared<TState>());
    }
    void Build(uint32_t id, std::shared_ptr<IState> state) {
        auto peers = Peers(id);
        if (StageVoterLearner && id != 4) peers.erase(4);
        for (auto& [_, peer] : peers) peer = std::make_shared<TEndpoint>([this](auto message) {
            if (message->Type == uint32_t(EMessageType::REQUEST_VOTE_REQUEST)) VoteTargets.push_back(message.template Cast<TMessageEx>()->Dst);
            Packets.push_back(std::move(message));
        });
        Processors[id - 1].reset();
        Nodes[id - 1] = std::make_shared<TRaft>(state, id, peers,
            StageVoterLearner && id != 4 ? std::vector<uint32_t>{} : std::vector<uint32_t>{4});
        Apps[id - 1] = std::make_shared<TApp>();
        Processors[id - 1] = std::make_unique<TRequestProcessor>(Nodes[id - 1], Apps[id - 1], peers);
    }
    void Drain(bool duplicate = false) {
        size_t budget = 10000;
        while (!Packets.empty()) {
            assert_true(budget-- > 0);
            auto message = Packets.front(); Packets.pop_front(); auto header = message.Cast<TMessageEx>();
            if (Isolated.contains(header->Src) || Isolated.contains(header->Dst)) continue;
            Nodes[header->Dst - 1]->Process(Now, message);
            if (duplicate) Nodes[header->Dst - 1]->Process(Now, message);
            Processors[header->Dst - 1]->ProcessCommitted();
        }
    }
    void Elect(uint32_t id) {
        Now += std::chrono::seconds(10); Nodes[id - 1]->ProcessTimeout(Now); Drain();
        Nodes[id - 1]->ProcessTimeout(Now); Drain();
        assert_int_equal(Nodes[id - 1]->CurrentStateName(), EState::LEADER);
    }
    void Pump(uint32_t leader = 1, int rounds = 40, bool duplicate = false) {
        for (int i = 0; i < rounds; ++i) {
            Now += std::chrono::milliseconds(100); Nodes[leader - 1]->ProcessTimeout(Now); Drain(duplicate);
            for (auto& processor : Processors) { processor->ProcessCommitted(); processor->ProcessWaiting(); }
        }
    }
    void Write(const std::string& key, const std::string& value) { Processors[0]->OnCommandRequest(Command(key, value), {}); Pump(); }
};
void test_invalid_learner_role_configurations_fail_before_binding(void**) {
    for (auto learners : {std::vector<uint32_t>{0}, std::vector<uint32_t>{4, 4},
         std::vector<uint32_t>{5}, std::vector<uint32_t>{1, 2, 3, 4}}) {
        auto state = std::make_shared<TState>();
        Reject([&] { TRaft raft(state, 1, Peers(1), learners); });
        assert_true(state->GetBoundVoters().empty()); assert_int_equal(state->CurrentTerm, 1);
    }
}
void test_learner_never_campaigns_votes_or_proposes(void**) {
    std::vector<TMessageHolder<TRequestVoteResponse>> replies;
    auto peers = Peers(4);
    peers[1] = std::make_shared<TEndpoint>([&](auto message) { replies.push_back(message.template Cast<TRequestVoteResponse>()); });
    auto state = std::make_shared<TState>(); TRaft learner(state, 4, peers, {4});
    for (int i = 0; i < 5; ++i) learner.ProcessTimeout(ITimeSource::Time{} + std::chrono::hours(i + 1));
    assert_int_equal(state->CurrentTerm, 1); assert_int_equal(state->VotedFor, 0);
    Reject([&] { learner.Become(EState::CANDIDATE); }); Reject([&] { learner.Become(EState::LEADER); });
    Reject([&] { learner.Append(NewHoldedMessage<TLogEntry>()); });
    auto request = NewHoldedMessage(TMessageEx{.Src = 1, .Dst = 4, .Term = 5},
        TRequestVoteRequest{.LastLogIndex = 0, .LastLogTerm = 0, .CandidateId = 1});
    learner.Process({}, request);
    assert_int_equal(replies.size(), 1); assert_false(replies[0]->VoteGranted);
    assert_int_equal(state->CurrentTerm, 5); assert_int_equal(state->VotedFor, 0);
    assert_int_equal(learner.CurrentStateName(), EState::FOLLOWER);
}
void test_learner_messages_cannot_change_voter_term_or_leadership(void**) {
    TCluster cluster; cluster.Elect(1); cluster.Pump();
    const auto term = cluster.Nodes[0]->GetState()->CurrentTerm;
    std::vector<TMessageHolder<TMessage>> messages;
    const TMessageEx header{.Src = 4, .Dst = 1, .Term = 99};
    messages.push_back(NewHoldedMessage(header, TRequestVoteRequest{.LastLogIndex = 0, .LastLogTerm = 0, .CandidateId = 4}));
    messages.push_back(NewHoldedMessage(header, TRequestVoteResponse{.VoteGranted = 1}));
    messages.push_back(NewHoldedMessage(header, TAppendEntriesRequest{.LeaderId = 4}));
    messages.push_back(NewHoldedMessage(header, TAppendEntriesResponse{.MatchIndex = 0, .Success = 0}));
    auto install = NewHoldedMessage<TInstallSnapshotRequest>(sizeof(TInstallSnapshotRequest) + 1);
    install->Src = 4; install->Dst = 1; install->Term = 99; install->SnapshotIndex = 1; install->SnapshotTerm = 1;
    install->TotalBytes = 44; install->LeaderId = 4; install->Data[0] = 'x'; messages.push_back(install);
    messages.push_back(NewHoldedMessage(header, TInstallSnapshotResponse{.SnapshotIndex = 1}));
    for (auto message : messages) {
        cluster.Nodes[0]->Process(cluster.Now, message);
        assert_int_equal(cluster.Nodes[0]->GetState()->CurrentTerm, term);
        assert_int_equal(cluster.Nodes[0]->CurrentStateName(), EState::LEADER);
    }
}
void test_voter_commit_quorums_ignore_arbitrary_learner_matches(void**) {
    TState state; state.Append(NewHoldedMessage<TLogEntry>());
    for (uint32_t count = 1; count <= 8; ++count) for (uint32_t acknowledgements = 0; acknowledgements < count; ++acknowledgements) {
        std::vector<uint32_t> voters;
        for (uint32_t id = 1; id <= count; ++id) voters.push_back(id);
        TVolatileState progress;
        for (uint32_t id = 2; id <= acknowledgements + 1; ++id) progress.MatchIndex[id] = 1;
        for (uint32_t id = count + 1; id <= count + 5; ++id) progress.MatchIndex[id] = 1;
        progress.CommitAdvance(voters, 1, state);
        assert_int_equal(progress.CommitIndex, acknowledgements + 1 > count / 2 ? 1 : 0);
    }
}
void test_single_voter_commits_and_reads_without_learner_acknowledgements(void**) {
    auto state = std::make_shared<TState>();
    auto raft = std::make_shared<TRaft>(state, 1, Peers(1), std::vector<uint32_t>{2, 3, 4});
    auto app = std::make_shared<TKv>(); TRequestProcessor processor(raft, app, {});
    raft->ProcessTimeout({}); assert_int_equal(raft->CurrentStateName(), EState::LEADER);
    assert_int_equal(raft->GetNservers(), 1); assert_int_equal(raft->GetNpeers(), 3);
    processor.OnCommandRequest(Command("a", "committed alone"), {});
    raft->ProcessTimeout({}); processor.ProcessCommitted();
    assert_true(Read(*app, "a") == "committed alone");
    size_t replies = 0;
    auto client = std::make_shared<TEndpoint>([&](auto message) {
        assert_int_equal(message.template Cast<TCommandResponse>()->ErrorCode, 0); ++replies;
    });
    auto read = NewHoldedMessage<TReadKv>(sizeof(TReadKv) + 1);
    read->KeySize = 1; read->Data[0] = 'a'; read->Flags = TCommandRequest::EConsistent;
    processor.OnCommandRequest(read, client); processor.ProcessWaiting(); assert_int_equal(replies, 1);
}
void test_learner_grants_cannot_elect_candidate_and_no_votes_are_solicited(void**) {
    TCluster cluster; cluster.Isolated = {2, 3};
    cluster.Nodes[0]->ProcessTimeout(cluster.Now); cluster.Drain();
    auto grant = NewHoldedMessage(TMessageEx{.Src = 4, .Dst = 1, .Term = cluster.Nodes[0]->GetState()->CurrentTerm},
        TRequestVoteResponse{.VoteGranted = 1});
    cluster.Nodes[0]->Process(cluster.Now, grant); cluster.Nodes[0]->ProcessTimeout(cluster.Now);
    assert_int_equal(cluster.Nodes[0]->CurrentStateName(), EState::CANDIDATE);
    assert_int_equal(cluster.Nodes[0]->GetMinVotes(), 2);
    for (auto id : cluster.VoteTargets) assert_true(id != 4);
    cluster.Isolated = {3}; cluster.Elect(1);
}
void test_learner_replication_cannot_commit_or_satisfy_strong_read(void**) {
    TCluster cluster; cluster.Elect(1); cluster.Pump();
    const auto before = cluster.Nodes[0]->GetVolatileState()->CommitIndex;
    cluster.Isolated = {2, 3}; cluster.Write("x", "new");
    assert_true(cluster.Nodes[0]->GetVolatileState()->MatchIndex.at(4) > before);
    assert_int_equal(cluster.Nodes[0]->GetVolatileState()->CommitIndex, before);
    assert_true(Read(*cluster.Apps[0], "x").empty()); assert_true(Read(*cluster.Apps[3], "x").empty());
    size_t replies = 0;
    auto client = std::make_shared<TEndpoint>([&](auto message) {
        auto reply = message.template Cast<TCommandResponse>();
        assert_int_equal(reply->Cookie, 77); assert_int_equal(reply->ErrorCode, 0); ++replies;
    });
    auto read = NewHoldedMessage<TReadKv>(sizeof(TReadKv) + 1);
    read->Flags = TCommandRequest::EConsistent; read->Cookie = 77; read->KeySize = 1; read->Data[0] = 'x';
    cluster.Processors[0]->OnCommandRequest(read, client); cluster.Pump(); assert_int_equal(replies, 0);
    cluster.Isolated = {3}; cluster.Pump(1, 80);
    assert_int_equal(replies, 1); assert_true(Read(*cluster.Apps[3], "x") == "new");
}
void test_learner_commit_ack_waits_for_commit_propagation(void**) {
    TCluster cluster; cluster.Elect(1); cluster.Pump();
    auto leader = cluster.Nodes[0];
    const auto index = leader->Append(cluster.Apps[0]->Prepare(Command("pending", "value")));

    // The learner can have the complete log while the voters cannot yet commit
    // this entry. Its ACK only proves the previous commit boundary.
    cluster.Isolated = {2, 3}; cluster.Pump(1, 40);
    assert_int_equal(leader->GetVolatileState()->CommitIndex, index - 1);
    assert_true(leader->GetVolatileState()->MatchIndex.at(4) >= index);
    assert_true(leader->IsLearnerCaughtUp(4));
    assert_true(leader->IsLearnerCommitAcknowledged(4));

    // Now let the voter majority commit while the learner is partitioned. The
    // log watermark remains true, but the correlated commit acknowledgement is
    // stale until the leader sends the new boundary and receives a reply.
    cluster.Isolated = {4}; cluster.Pump(1, 40);
    assert_int_equal(leader->GetVolatileState()->CommitIndex, index);
    assert_true(leader->IsLearnerCaughtUp(4));
    assert_false(leader->IsLearnerCommitAcknowledged(4));

    cluster.Isolated.clear(); cluster.Pump(1, 40);
    assert_true(leader->IsLearnerCommitAcknowledged(4));
    assert_int_equal(cluster.Apps[3]->LastAppliedIndex, index);
}
void test_learner_snapshot_catchup_status_and_restart_preserve_role(void**) {
    TCluster cluster; cluster.Isolated = {4}; cluster.Elect(1);
    for (const std::string key : {"a", "b", "c"}) cluster.Write(key, std::string(60000, key[0]));
    auto snapshot = cluster.Nodes[0]->Compact();
    assert_true(snapshot.Voters == (std::vector<uint32_t>{1, 2, 3}));
    assert_false(cluster.Nodes[0]->IsLearnerCaughtUp(4)); cluster.Isolated.clear(); cluster.Pump(1, 100, true);
    assert_int_equal(cluster.Apps[3]->Loads, 1); assert_true(cluster.Nodes[0]->IsLearnerCaughtUp(4));
    assert_true(cluster.Nodes[0]->IsLearnerCommitAcknowledged(4));
    assert_false(cluster.Nodes[0]->IsLearnerCaughtUp(2)); assert_false(cluster.Nodes[0]->IsLearnerCaughtUp(99));
    assert_true(Read(*cluster.Apps[3], "c") == std::string(60000, 'c'));
    cluster.Nodes[0]->Append(cluster.Apps[0]->Prepare(Command("tail", "later")));
    assert_false(cluster.Nodes[0]->IsLearnerCaughtUp(4)); cluster.Pump();
    assert_true(cluster.Nodes[0]->IsLearnerCaughtUp(4));
    auto state = cluster.Nodes[3]->GetState(); cluster.Build(4, state);
    cluster.Nodes[3]->ProcessTimeout(cluster.Now + std::chrono::hours(1)); cluster.Pump();
    assert_true(Read(*cluster.Apps[3], "tail") == "later");
    assert_false(cluster.Nodes[3]->IsVoter(4)); assert_int_equal(state->VotedFor, 0);
    assert_int_equal(cluster.Nodes[3]->CurrentStateName(), EState::FOLLOWER);
}
void test_restart_cannot_promote_learner_or_change_voters_but_can_add_transport(void**) {
    auto state = std::make_shared<TState>(); TRaft learner(state, 4, Peers(4), {4});
    const auto fields = static_cast<TStateFields>(*state);
    Reject([&] { TRaft promoted(state, 4, Peers(4)); });
    assert_true(static_cast<TStateFields>(*state) == fields);
    TRaft expanded(state, 4, Peers(4, 5), {4, 5});
    assert_true(expanded.GetVoterIds() == (std::vector<uint32_t>{1, 2, 3}));
    assert_true(expanded.GetLearnerIds() == (std::vector<uint32_t>{4, 5}));
    Reject([&] { state->BindVoters({1, 2, 3, 4}); });
    Reject([&] { state->InstallSnapshot({1, 1, {1, 2, 3, 4}, "changed voters"}); });
}
void test_nonfresh_unbound_store_requires_snapshot_proof_for_learners(void**) {
    auto state = std::make_shared<TState>();
    state->CurrentTerm = 2; state->VotedFor = 1; state->Append(NewHoldedMessage<TLogEntry>());
    Reject([&] { TRaft learnerAware(state, 1, Peers(1), {4}); });
    assert_true(state->GetBoundVoters().empty());
    TKv image; state->InstallSnapshot({1, 1, {1, 2, 3}, image.SaveSnapshot()});
    TRaft adopted(state, 1, Peers(1), {4});
    assert_true(state->GetBoundVoters() == (std::vector<uint32_t>{1, 2, 3}));
}
void test_online_learner_staging_snapshot_catchup_and_quorum_isolation(void**) {
    TCluster cluster(true); cluster.Elect(1);
    for (const std::string key : {"a", "b", "c"}) cluster.Write(key, std::string(60000, key[0]));
    auto leader = cluster.Nodes[0]; const auto fixed = leader->GetVoterIds();
    const auto snapshot = leader->Compact(); assert_true(snapshot.Index > 1);
    auto endpoint = std::make_shared<TEndpoint>([&](auto message) { cluster.Packets.push_back(std::move(message)); });
    const auto fields = static_cast<TStateFields>(*leader->GetState());
    Reject([&] { leader->AddLearnerTarget(2, endpoint); });
    leader->AddLearnerTarget(4, endpoint);
    assert_true(static_cast<TStateFields>(*leader->GetState()) == fields);
    assert_true(leader->GetState()->GetBoundVoters() == fixed);
    assert_true(leader->GetVoterIds() == fixed); assert_int_equal(leader->GetNservers(), 3);
    assert_int_equal(leader->GetNpeers(), 3); assert_false(leader->IsLearnerCaughtUp(4));
    assert_true(leader->GetLearnerIds() == std::vector<uint32_t>({4}));
    cluster.Pump(1, 120, true);
    assert_true(leader->IsLearnerCaughtUp(4)); assert_int_equal(cluster.Apps[3]->Loads, 1);
    assert_true(Read(*cluster.Apps[3], "c") == std::string(60000, 'c'));
    Reject([&] { TRaft promoted(leader->GetState(), 1, Peers(1)); });
    assert_true(leader->GetState()->GetBoundVoters() == fixed);
    const auto before = leader->GetVolatileState()->CommitIndex;
    cluster.Isolated = {2, 3}; cluster.Write("uncommitted", "value");
    assert_true(leader->GetVolatileState()->MatchIndex.at(4) > before);
    assert_int_equal(leader->GetVolatileState()->CommitIndex, before);
    assert_true(Read(*cluster.Apps[0], "uncommitted").empty());
    size_t replies = 0;
    auto client = std::make_shared<TEndpoint>([&](auto message) {
        assert_int_equal(message.template Cast<TCommandResponse>()->ErrorCode, 0); ++replies;
    });
    auto read = NewHoldedMessage<TReadKv>(sizeof(TReadKv) + 1);
    read->Flags = TCommandRequest::EConsistent; read->KeySize = 1; read->Data[0] = 'a';
    cluster.Processors[0]->OnCommandRequest(read, client); cluster.Pump();
    assert_int_equal(replies, 0);
    cluster.Isolated = {3}; cluster.Pump(1, 80);
    assert_int_equal(replies, 1);
    assert_true(Read(*cluster.Apps[0], "uncommitted") == "value");
    assert_true(leader->IsLearnerCaughtUp(4));
}
void test_online_learner_removal_and_stale_reply_after_readdition(void**) {
    TCluster cluster(true); cluster.Elect(1); cluster.Pump();
    auto leader = cluster.Nodes[0];
    auto endpoint = std::make_shared<TEndpoint>([&](auto message) { cluster.Packets.push_back(std::move(message)); });
    leader->AddLearnerTarget(4, endpoint); cluster.Pump(); assert_true(leader->IsLearnerCaughtUp(4));
    leader->ApproveRead();
    TMessageHolder<TMessage> request;
    for (auto it = cluster.Packets.begin(); it != cluster.Packets.end(); ++it) {
        auto header = it->Cast<TMessageEx>();
        if (header->Src == 1 && header->Dst == 4 && header->Type == uint32_t(EMessageType::APPEND_ENTRIES_REQUEST)) {
            request = *it; cluster.Packets.erase(it); break;
        }
    }
    assert_true(bool(request));
    cluster.Nodes[3]->Process(cluster.Now, request);
    TMessageHolder<TMessage> delayed;
    for (auto it = cluster.Packets.begin(); it != cluster.Packets.end(); ++it) {
        auto header = it->Cast<TMessageEx>();
        if (header->Src == 4 && header->Dst == 1 && header->Type == uint32_t(EMessageType::APPEND_ENTRIES_RESPONSE)) {
            delayed = *it; cluster.Packets.erase(it); break;
        }
    }
    assert_true(bool(delayed)); cluster.Packets.clear();
    assert_true(leader->RemoveLearnerTarget(4) == endpoint);
    assert_true(leader->GetLearnerIds().empty()); assert_int_equal(leader->GetNpeers(), 2);
    assert_false(leader->IsLearnerCaughtUp(4));
    leader->Process(cluster.Now, delayed); // no longer a configured peer
    leader->AddLearnerTarget(4, endpoint);
    cluster.Now += std::chrono::seconds(3); leader->ProcessTimeout(cluster.Now);
    leader->Process(cluster.Now, delayed); // old RPC identity cannot confirm the new target
    assert_false(leader->IsLearnerCaughtUp(4));
    assert_int_equal(leader->GetVolatileState()->MatchIndex.at(4), 0);
    cluster.Drain(); cluster.Pump(); assert_true(leader->IsLearnerCaughtUp(4));
    assert_true(leader->GetVoterIds() == std::vector<uint32_t>({1, 2, 3}));
}
void test_online_learner_removal_during_snapshot_and_delayed_ack(void**) {
    TCluster cluster(true); cluster.Elect(1);
    for (const std::string key : {"a", "b", "c"}) cluster.Write(key, std::string(60000, key[0]));
    auto leader = cluster.Nodes[0]; leader->Compact();
    auto endpoint = std::make_shared<TEndpoint>([&](auto message) { cluster.Packets.push_back(std::move(message)); });
    const auto nextSnapshot = [&]() -> TMessageHolder<TMessage> {
        for (int attempt = 0; attempt < 40; ++attempt) {
            cluster.Now += std::chrono::seconds(3); leader->ProcessTimeout(cluster.Now);
            size_t budget = 1000;
            while (!cluster.Packets.empty()) {
                assert_true(budget-- > 0);
                auto message = cluster.Packets.front(); cluster.Packets.pop_front();
                auto header = message.Cast<TMessageEx>();
                if (header->Src == 1 && header->Dst == 4 && header->Type == uint32_t(EMessageType::INSTALL_SNAPSHOT_REQUEST))
                    return message;
                cluster.Nodes[header->Dst - 1]->Process(cluster.Now, message);
                cluster.Processors[header->Dst - 1]->ProcessCommitted();
            }
        }
        fail_msg("Learner did not enter snapshot transfer"); return {};
    };
    leader->AddLearnerTarget(4, endpoint);
    auto first = nextSnapshot(); const auto firstSeq = first.Cast<TMessageEx>()->Seqno;
    cluster.Nodes[3]->Process(cluster.Now, first);
    TMessageHolder<TMessage> delayed;
    for (auto it = cluster.Packets.begin(); it != cluster.Packets.end(); ++it) {
        auto header = it->Cast<TMessageEx>();
        if (header->Src == 4 && header->Dst == 1 && header->Type == uint32_t(EMessageType::INSTALL_SNAPSHOT_RESPONSE)) {
            delayed = *it; cluster.Packets.erase(it); break;
        }
    }
    assert_true(bool(delayed)); assert_false(delayed.Cast<TInstallSnapshotResponse>()->Installed);
    assert_true(leader->GetVolatileState()->SnapshotRpcs.contains(4));
    assert_true(leader->RemoveLearnerTarget(4) == endpoint);
    assert_false(leader->GetVolatileState()->SnapshotRpcs.contains(4)); cluster.Packets.clear();
    leader->AddLearnerTarget(4, endpoint);
    auto replacement = nextSnapshot(); const auto replacementSeq = replacement.Cast<TMessageEx>()->Seqno;
    assert_true(replacementSeq != firstSeq);
    leader->Process(cluster.Now, delayed);
    const auto& pending = leader->GetVolatileState()->SnapshotRpcs.at(4);
    assert_int_equal(pending.Seqno, replacementSeq); assert_int_equal(pending.Offset, 0);
    cluster.Nodes[3]->Process(cluster.Now, replacement); cluster.Drain(); cluster.Pump(1, 120, true);
    assert_true(leader->IsLearnerCaughtUp(4)); assert_int_equal(cluster.Apps[3]->Loads, 1);
    assert_true(Read(*cluster.Apps[3], "c") == std::string(60000, 'c'));
}
void test_online_learner_requires_reconciliation_after_leader_change(void**) {
    TCluster cluster(true); cluster.Elect(1);
    auto endpoint = std::make_shared<TEndpoint>([&](auto message) { cluster.Packets.push_back(std::move(message)); });
    cluster.Nodes[0]->AddLearnerTarget(4, endpoint); cluster.Pump();
    assert_true(cluster.Nodes[0]->IsLearnerCaughtUp(4));
    cluster.Isolated = {1}; cluster.Elect(2); cluster.Pump(2);
    assert_true(cluster.Nodes[1]->GetLearnerIds().empty());
    assert_false(cluster.Nodes[1]->IsLearnerCaughtUp(4));
    cluster.Nodes[1]->AddLearnerTarget(4, endpoint); cluster.Pump(2);
    assert_true(cluster.Nodes[1]->IsLearnerCaughtUp(4));
    cluster.Processors[1]->OnCommandRequest(Command("after", "failover"), {}); cluster.Pump(2, 80);
    assert_true(Read(*cluster.Apps[3], "after") == "failover");
    assert_true(cluster.Nodes[1]->GetVoterIds() == std::vector<uint32_t>({1, 2, 3}));
    cluster.Isolated.clear(); cluster.Pump(2, 80);
    assert_int_equal(cluster.Nodes[0]->CurrentStateName(), EState::FOLLOWER);
    assert_true(cluster.Nodes[0]->GetLearnerIds() == std::vector<uint32_t>({4}));
}
void test_online_learner_staging_rejects_invalid_calls(void**) {
    auto state = std::make_shared<TState>(); auto raft = std::make_shared<TRaft>(state, 1, Peers(1, 3));
    auto endpoint = std::make_shared<TEndpoint>();
    Reject([&] { raft->AddLearnerTarget(4, endpoint); });
    Reject([&] { raft->RemoveLearnerTarget(2); });
    raft->ProcessTimeout({}); // candidate; no remote votes
    Reject([&] { raft->AddLearnerTarget(4, endpoint); });
    auto single = std::make_shared<TRaft>(std::make_shared<TState>(), 1, TNodeDict{});
    single->ProcessTimeout({}); assert_int_equal(single->CurrentStateName(), EState::LEADER);
    for (uint32_t id : {0u, 1u}) Reject([&] { single->AddLearnerTarget(id, endpoint); });
    Reject([&] { single->AddLearnerTarget(2, {}); });
    single->AddLearnerTarget(2, endpoint);
    Reject([&] { single->AddLearnerTarget(2, endpoint); });
    Reject([&] { single->RemoveLearnerTarget(1); });
    Reject([&] { single->RemoveLearnerTarget(3); });
    assert_true(single->GetVoterIds() == std::vector<uint32_t>({1}));
    assert_int_equal(single->GetNservers(), 1); assert_int_equal(single->GetNpeers(), 1);
    auto unbound = std::make_shared<TState>(); unbound->CurrentTerm = 2;
    unbound->Append(NewHoldedMessage<TLogEntry>());
    TRaft oldStore(unbound, 1, {}); oldStore.ProcessTimeout({});
    assert_int_equal(oldStore.CurrentStateName(), EState::LEADER);
    assert_true(unbound->GetBoundVoters().empty());
    Reject([&] { oldStore.AddLearnerTarget(2, endpoint); });
    assert_true(oldStore.GetLearnerIds().empty()); assert_int_equal(oldStore.GetNpeers(), 0);
}
void test_recovered_hard_state_must_match_fixed_voters(void**) {
    for (int mode = 0; mode < 2; ++mode) {
        auto state = std::make_shared<TState>();
        state->CurrentTerm = mode == 0 ? 2 : 0; state->VotedFor = mode == 0 ? 4 : 0;
        const auto before = static_cast<TStateFields>(*state);
        Reject([&] { TRaft invalid(state, 1, Peers(1, 3)); });
        assert_true(static_cast<TStateFields>(*state) == before); assert_true(state->GetBoundVoters().empty());
    }
    for (uint32_t vote : {0u, 1u, 2u}) {
        auto state = std::make_shared<TState>(); state->CurrentTerm = 2; state->VotedFor = vote;
        TRaft valid(state, 1, Peers(1, 3)); assert_int_equal(state->VotedFor, vote);
    }
}
#ifdef __linux__
struct TFiles {
    std::filesystem::path Directory; std::string Name;
    TFiles() {
        static unsigned next = 0;
        do {
            Directory = std::filesystem::temp_directory_path() /
                ("miniraft-learners-" + std::to_string(getpid()) + "-" + std::to_string(next++));
        } while (std::filesystem::exists(Directory));
        assert_true(std::filesystem::create_directory(Directory)); Name = (Directory / "raft").string();
    }
    ~TFiles() { std::error_code ignored; std::filesystem::remove_all(Directory, ignored); }
};
void test_legacy_recovered_vote_outside_voters_preserves_storage(void**) {
    TFiles files;
    { TDiskState state(files.Name, 1); state.CurrentTerm = 2; state.VotedFor = 4; state.Commit(); }
    std::vector<std::string> before;
    for (auto suffix : {".entries.1", ".index.1", ".state.1"}) {
        std::ifstream input(files.Name + suffix, std::ios::binary);
        before.emplace_back(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    { auto state = std::make_shared<TDiskState>(files.Name, 1);
      const auto fields = static_cast<TStateFields>(*state);
      Reject([&] { TRaft invalid(state, 1, Peers(1, 3)); });
      assert_true(static_cast<TStateFields>(*state) == fields); }
    size_t i = 0;
    for (auto suffix : {".entries.1", ".index.1", ".state.1"}) {
        std::ifstream input(files.Name + suffix, std::ios::binary);
        assert_true(std::string(std::istreambuf_iterator<char>(input), {}) == before[i++]);
    }
}
void test_durable_voter_binding_is_atomic_before_first_vote(void**) {
    using TStage = TDurableState::EStage;
    for (auto stage : {TStage::TempCreated, TStage::Written, TStage::FileSynced, TStage::Renamed, TStage::DirectorySynced}) {
        TFiles files;
        auto child = fork(); assert_true(child >= 0);
        if (!child) {
            bool armed = false;
            try { auto state = std::make_shared<TDurableState>(files.Name, 4, [&](auto point) { if (armed && point == stage) _exit(90); });
                armed = true;
                TRaft raft(state, 4, Peers(4), {4});
            } catch (...) { _exit(91); } _exit(92);
        }
        int status = 0; assert_int_equal(waitpid(child, &status, 0), child);
        assert_true(WIFEXITED(status)); assert_int_equal(WEXITSTATUS(status), 90);
        TDurableState state(files.Name, 4);
        if (stage >= TStage::Renamed) assert_true(state.GetBoundVoters() == (std::vector<uint32_t>{1, 2, 3}));
        else assert_true(state.GetBoundVoters().empty());
        assert_int_equal(state.CurrentTerm, 1); assert_int_equal(state.VotedFor, 0); assert_int_equal(state.LastLogIndex, 0);
    }
}
void test_trusted_legacy_voter_baseline_migrates_durable_state(void**) {
    {
        TFiles files;
        {
            TDurableState fresh(files.Name, 1);
            Reject([&] { fresh.BindVotersFromTrustedBaseline({1, 2, 3}); });
        }
        auto recovered = std::make_shared<TDurableState>(files.Name, 1);
        assert_int_equal(recovered->CurrentTerm, 1);
        assert_int_equal(recovered->LastLogIndex, 0);
        Reject([&] { TRaft missingBaseline(recovered, 1, Peers(1, 3)); });
        recovered->BindVotersFromTrustedBaseline({1, 2, 3});
        assert_true(recovered->GetBoundVoters() == (std::vector<uint32_t>{1, 2, 3}));
        TRaft raft(recovered, 1, Peers(1, 3));
        assert_true(raft.GetVoterIds() == (std::vector<uint32_t>{1, 2, 3}));
    }
    {
        TFiles files; TStateFields before;
        {
            TDurableState legacy(files.Name, 1);
            legacy.CurrentTerm = 3; legacy.VotedFor = 1;
            auto entry = NewHoldedMessage<TLogEntry>(); entry->Term = 2;
            legacy.Append(entry); before = legacy;
        }
        const auto path = files.Name + ".durable.1";
        std::ifstream originalInput(path, std::ios::binary);
        const std::string original(std::istreambuf_iterator<char>(originalInput), {});
        assert_true(original.substr(0, 8) == "MRFTD004");

        {
            auto state = std::make_shared<TDurableState>(files.Name, 1);
            assert_true(state->GetBoundVoters().empty());
            Reject([&] { state->BindVoters({1, 2, 3}); });
            Reject([&] { state->BindVotersFromTrustedBaseline({2, 3}); });
            assert_true(static_cast<TStateFields>(*state) == before);
            assert_true(state->GetBoundVoters().empty());
            std::ifstream unchanged(path, std::ios::binary);
            assert_true(std::string(std::istreambuf_iterator<char>(unchanged), {}) == original);

            state->BindVotersFromTrustedBaseline({1, 2, 3});
            assert_true(static_cast<TStateFields>(*state) == before);
            assert_true(state->GetBoundVoters() == (std::vector<uint32_t>{1, 2, 3}));
            assert_int_equal(state->RetainedLogTerm(1), 2);
        }
        { std::ifstream migrated(path, std::ios::binary); std::string magic(8, '\0');
          migrated.read(magic.data(), magic.size()); assert_true(magic == "MRFTD004"); }
        auto recovered = std::make_shared<TDurableState>(files.Name, 1);
        assert_true(static_cast<TStateFields>(*recovered) == before);
        assert_true(recovered->GetBoundVoters() == (std::vector<uint32_t>{1, 2, 3}));
        TRaft raft(recovered, 1, Peers(1, 3));
        assert_true(raft.GetVoterIds() == (std::vector<uint32_t>{1, 2, 3}));
    }
    {
        TFiles files; TStateFields before; std::string application;
        {
            TDurableState legacy(files.Name, 1); TKv image;
            legacy.CurrentTerm = 3; legacy.VotedFor = 1;
            auto entry = image.Prepare(Command("migrated", "value")); entry->Term = 2;
            legacy.Append(entry); image.Write(entry, 1);
            application = image.SaveSnapshot();
            legacy.InstallSnapshot({1, 2, {1, 2, 3}, application});
            auto tail = NewHoldedMessage<TLogEntry>(); tail->Term = 3; legacy.Append(tail);
            before = legacy;
        }
        const auto path = files.Name + ".durable.1";
        std::ifstream originalInput(path, std::ios::binary);
        const std::string original(std::istreambuf_iterator<char>(originalInput), {});
        assert_true(original.substr(0, 8) == "MRFTD004");

        {
            auto state = std::make_shared<TDurableState>(files.Name, 1);
            Reject([&] { state->BindVotersFromTrustedBaseline({1, 2}); });
            assert_true(static_cast<TStateFields>(*state) == before);
            assert_true(state->GetBoundVoters().empty());
            std::ifstream unchanged(path, std::ios::binary);
            assert_true(std::string(std::istreambuf_iterator<char>(unchanged), {}) == original);
            state->BindVotersFromTrustedBaseline({1, 2, 3});
            assert_true(static_cast<TStateFields>(*state) == before);
            assert_true(state->GetSnapshot()->Data == application);
            assert_true(state->GetBoundVoters() == (std::vector<uint32_t>{1, 2, 3}));
        }
        { std::ifstream migrated(path, std::ios::binary); std::string magic(8, '\0');
          migrated.read(magic.data(), magic.size()); assert_true(magic == "MRFTD004"); }
        auto recovered = std::make_shared<TDurableState>(files.Name, 1);
        assert_true(static_cast<TStateFields>(*recovered) == before);
        assert_true(recovered->GetBoundVoters() == (std::vector<uint32_t>{1, 2, 3}));
        auto raft = std::make_shared<TRaft>(recovered, 1, Peers(1, 3));
        auto app = std::make_shared<TKv>(); TRequestProcessor processor(raft, app, {});
        assert_true(Read(*app, "migrated") == "value");
        assert_true(bool(recovered->Get(2)));
    }
}
void test_trusted_legacy_voter_baseline_refuses_configuration_logs(void**) {
    TFiles files;
    {
        TDurableState legacy(files.Name, 1);
        legacy.CurrentTerm = 2;
        auto entry = NewHoldedMessage<TLogEntry>(); entry->Term = 2; entry->Flags = TLogEntry::EConfiguration;
        legacy.Append(entry);
    }
    const auto path = files.Name + ".durable.1";
    std::ifstream originalInput(path, std::ios::binary);
    const std::string original(std::istreambuf_iterator<char>(originalInput), {});
    auto state = std::make_shared<TDurableState>(files.Name, 1);
    Reject([&] { state->BindVotersFromTrustedBaseline({1, 2, 3}); });
    assert_true(state->GetBoundVoters().empty()); assert_int_equal(state->LastLogIndex, 1);
    std::ifstream unchanged(path, std::ios::binary);
    assert_true(std::string(std::istreambuf_iterator<char>(unchanged), {}) == original);
}
void test_trusted_legacy_voter_baseline_must_precede_raft_startup(void**) {
    TFiles files;
    {
        TDurableState legacy(files.Name, 1);
        legacy.CurrentTerm = 2; legacy.VotedFor = 1;
        legacy.Commit();
    }
    const auto path = files.Name + ".durable.1";
    std::ifstream originalInput(path, std::ios::binary);
    const std::string original(std::istreambuf_iterator<char>(originalInput), {});
    auto state = std::make_shared<TDurableState>(files.Name, 1);
    Reject([&] { TRaft missingBaseline(state, 1, Peers(1, 3)); });
    assert_true(state->GetBoundVoters().empty());
    assert_int_equal(state->VotedFor, 1);
    std::ifstream unchanged(path, std::ios::binary);
    assert_true(std::string(std::istreambuf_iterator<char>(unchanged), {}) == original);

    state->BindVotersFromTrustedBaseline({1, 2, 3});
    auto raft = std::make_shared<TRaft>(state, 1, Peers(1, 3));
    assert_true(raft->GetVoterIds() == (std::vector<uint32_t>{1, 2, 3}));
    assert_true(state->GetBoundVoters() == (std::vector<uint32_t>{1, 2, 3}));
    std::ifstream migratedInput(path, std::ios::binary);
    const std::string migrated(std::istreambuf_iterator<char>(migratedInput), {});
    Reject([&] { state->BindVotersFromTrustedBaseline({1, 2, 3}); });
    std::ifstream after(path, std::ios::binary);
    assert_true(std::string(std::istreambuf_iterator<char>(after), {}) == migrated);
}
void test_trusted_legacy_voter_binding_is_atomic_at_each_publish_stage(void**) {
    using TStage = TDurableState::EStage;
    const std::vector<TStage> stages = {TStage::TempCreated, TStage::Written, TStage::FileSynced,
        TStage::Renamed, TStage::DirectorySynced};
    for (bool withSnapshot : {false, true}) {
        for (auto stage : stages) {
            TFiles files;
            {
                TDurableState legacy(files.Name, 1); legacy.CurrentTerm = 3; legacy.VotedFor = 1;
                if (withSnapshot) {
                    TKv image; auto entry = image.Prepare(Command("atomic", "snapshot"));
                    entry->Term = 2; legacy.Append(entry); image.Write(entry, 1);
                    legacy.InstallSnapshot({1, 2, {1, 2, 3}, image.SaveSnapshot()});
                    auto tail = NewHoldedMessage<TLogEntry>(); tail->Term = 3; legacy.Append(tail);
                } else {
                    auto entry = NewHoldedMessage<TLogEntry>(); entry->Term = 2; legacy.Append(entry);
                }
            }
            auto child = fork(); assert_true(child >= 0);
            if (!child) {
                try {
                    TDurableState state(files.Name, 1, [&](auto point) {
                        if (point == stage) _exit(90);
                    });
                    state.BindVotersFromTrustedBaseline({1, 2, 3});
                } catch (...) { _exit(91); }
                _exit(92);
            }
            int status = 0; assert_int_equal(waitpid(child, &status, 0), child);
            assert_true(WIFEXITED(status)); assert_int_equal(WEXITSTATUS(status), 90);

            TDurableState recovered(files.Name, 1);
            if (stage < TStage::Renamed) {
                assert_true(recovered.GetBoundVoters().empty());
                assert_int_equal(recovered.LastLogIndex, withSnapshot ? 2 : 1);
            } else {
                assert_true(recovered.GetBoundVoters() == (std::vector<uint32_t>{1, 2, 3}));
                assert_int_equal(recovered.LastLogIndex, withSnapshot ? 2 : 1);
            }
            if (withSnapshot) {
                assert_true(bool(recovered.GetSnapshot()));
                assert_int_equal(recovered.SnapshotIndex, 1);
                assert_int_equal(recovered.SnapshotTerm, 2);
            }
        }
    }
}
void test_durable_learner_snapshot_suffix_and_role_survive_restart(void**) {
    TFiles files;
    {
        auto state = std::make_shared<TDurableState>(files.Name, 4); TRaft raft(state, 4, Peers(4), {4});
        TKv image; auto entry = image.Prepare(Command("a", "snapshot")); state->Append(entry); image.Write(entry, 1);
        state->InstallSnapshot({1, 1, {1, 2, 3}, image.SaveSnapshot()});
        state->Append(image.Prepare(Command("b", "tail")));
    }
    auto state = std::make_shared<TDurableState>(files.Name, 4);
    Reject([&] { TRaft promoted(state, 4, Peers(4)); });
    auto raft = std::make_shared<TRaft>(state, 4, Peers(4), std::vector<uint32_t>{4});
    auto app = std::make_shared<TKv>(); TRequestProcessor processor(raft, app, {});
    assert_true(Read(*app, "a") == "snapshot"); assert_true(Read(*app, "b").empty());
    auto heartbeat = NewHoldedMessage(TMessageEx{.Src = 1, .Dst = 4, .Term = 1},
        TAppendEntriesRequest{.PrevLogIndex = 2, .PrevLogTerm = 1, .LeaderCommit = 2, .LeaderId = 1});
    raft->Process({}, heartbeat); processor.ProcessCommitted();
    assert_true(Read(*app, "b") == "tail"); assert_int_equal(state->VotedFor, 0);
    assert_true(state->GetBoundVoters() == (std::vector<uint32_t>{1, 2, 3}));
}
void test_durable_v4_corrupt_commit_or_roles_fail_without_rewriting(void**) {
    for (size_t offset : {size_t(40), size_t(52), size_t(56), size_t(64)}) {
        TFiles files;
        { auto state = std::make_shared<TDurableState>(files.Name, 4); TRaft raft(state, 4, Peers(4), {4}); }
        const auto path = files.Name + ".durable.4";
        std::ifstream input(path, std::ios::binary); std::string bytes(std::istreambuf_iterator<char>(input), {}); input.close();
        assert_true(bytes.substr(0, 8) == "MRFTD004");
        const auto width = offset == 40 || offset == 64 ? 8 : 4;
        for (size_t i = offset; i < offset + width; ++i) bytes[i] = offset == 52 || offset == 56 ? 0 : char(0xff);
        const auto crc = NSnapshotCodec::Checksum(bytes.substr(0, 8) + bytes.substr(20));
        for (int i = 0; i < 4; ++i) bytes[16 + i] = char(crc >> (8 * i));
        { std::ofstream out(path, std::ios::binary); out.write(bytes.data(), bytes.size()); }
        Reject([&] { TDurableState invalid(files.Name, 4); });
        std::ifstream after(path, std::ios::binary); assert_true(std::string(std::istreambuf_iterator<char>(after), {}) == bytes);
    }
}
void test_durable_snapshot_adoption_preserves_vote_log_and_application(void**) {
    TFiles files; TStateFields before; std::string application;
    {
        TDurableState state(files.Name, 1); TKv app;
        state.CurrentTerm = 3; state.VotedFor = 1;
        auto entry = app.Prepare(Command("a", "snapshot")); state.Append(entry); app.Write(entry, 1);
        application = app.SaveSnapshot(); state.InstallSnapshot({1, 1, {1, 2, 3}, application});
        state.Append(app.Prepare(Command("b", "tail"))); before = state;
    }
    const auto path = files.Name + ".durable.1";
    { std::ifstream input(path, std::ios::binary); std::string magic(8, '\0'); input.read(magic.data(), 8); assert_true(magic == "MRFTD004"); }
    {
        auto state = std::make_shared<TDurableState>(files.Name, 1);
        auto raft = std::make_shared<TRaft>(state, 1, Peers(1), std::vector<uint32_t>{4});
        assert_true(static_cast<TStateFields>(*state) == before);
        assert_true(state->GetSnapshot()->Data == application); assert_true(bool(state->Get(2)));
        auto app = std::make_shared<TKv>(); TRequestProcessor processor(raft, app, {});
        assert_true(Read(*app, "a") == "snapshot"); assert_true(Read(*app, "b").empty());
        Reject([&] { TRaft promoted(state, 1, Peers(1)); });
    }
    TDurableState state(files.Name, 1);
    assert_true(static_cast<TStateFields>(state) == before);
    assert_true(state.GetSnapshot()->Data == application);
    assert_true(state.GetBoundVoters() == (std::vector<uint32_t>{1, 2, 3}));
}
void test_legacy_disk_backend_cannot_enable_learner_roles(void**) {
    TFiles files; auto state = std::make_shared<TDiskState>(files.Name, 4);
    Reject([&] { TRaft raft(state, 4, Peers(4), {4}); });
    assert_int_equal(state->CurrentTerm, 1); assert_int_equal(state->VotedFor, 0);
}
void test_legacy_leader_cannot_stage_online_learner(void**) {
    TFiles files; auto state = std::make_shared<TDiskState>(files.Name, 1);
    TRaft raft(state, 1, {}); raft.ProcessTimeout({});
    assert_int_equal(raft.CurrentStateName(), EState::LEADER);
    const auto fields = static_cast<TStateFields>(*state);
    Reject([&] { raft.AddLearnerTarget(2, std::make_shared<TEndpoint>()); });
    assert_true(static_cast<TStateFields>(*state) == fields);
    assert_true(raft.GetLearnerIds().empty()); assert_int_equal(raft.GetNpeers(), 0);
}
void test_online_learner_staging_does_not_change_durable_binding(void**) {
    TFiles files; const auto path = files.Name + ".durable.1";
    {
        auto state = std::make_shared<TDurableState>(files.Name, 1); TRaft raft(state, 1, {});
        raft.ProcessTimeout({}); assert_int_equal(raft.CurrentStateName(), EState::LEADER);
        std::ifstream input(path, std::ios::binary); std::string before(std::istreambuf_iterator<char>(input), {}); input.close();
        auto endpoint = std::make_shared<TEndpoint>(); raft.AddLearnerTarget(2, endpoint);
        assert_true(state->GetBoundVoters() == std::vector<uint32_t>({1}));
        std::ifstream afterAdd(path, std::ios::binary);
        assert_true(std::string(std::istreambuf_iterator<char>(afterAdd), {}) == before);
        assert_true(raft.RemoveLearnerTarget(2) == endpoint);
        std::ifstream afterRemove(path, std::ios::binary);
        assert_true(std::string(std::istreambuf_iterator<char>(afterRemove), {}) == before);
        raft.AddLearnerTarget(2, endpoint); // Leave staged at process shutdown.
        std::ifstream afterReadd(path, std::ios::binary);
        assert_true(std::string(std::istreambuf_iterator<char>(afterReadd), {}) == before);
    }
    auto recovered = std::make_shared<TDurableState>(files.Name, 1);
    TRaft raft(recovered, 1, {});
    assert_true(raft.GetLearnerIds().empty()); assert_true(recovered->GetBoundVoters() == std::vector<uint32_t>({1}));
}
#endif
}
int main() {
    const CMUnitTest tests[] = {
        cmocka_unit_test(test_invalid_learner_role_configurations_fail_before_binding),
        cmocka_unit_test(test_learner_never_campaigns_votes_or_proposes),
        cmocka_unit_test(test_learner_messages_cannot_change_voter_term_or_leadership),
        cmocka_unit_test(test_voter_commit_quorums_ignore_arbitrary_learner_matches),
        cmocka_unit_test(test_single_voter_commits_and_reads_without_learner_acknowledgements),
        cmocka_unit_test(test_learner_grants_cannot_elect_candidate_and_no_votes_are_solicited),
        cmocka_unit_test(test_learner_replication_cannot_commit_or_satisfy_strong_read),
        cmocka_unit_test(test_learner_commit_ack_waits_for_commit_propagation),
        cmocka_unit_test(test_learner_snapshot_catchup_status_and_restart_preserve_role),
        cmocka_unit_test(test_restart_cannot_promote_learner_or_change_voters_but_can_add_transport),
        cmocka_unit_test(test_nonfresh_unbound_store_requires_snapshot_proof_for_learners),
        cmocka_unit_test(test_online_learner_staging_snapshot_catchup_and_quorum_isolation),
        cmocka_unit_test(test_online_learner_removal_and_stale_reply_after_readdition),
        cmocka_unit_test(test_online_learner_removal_during_snapshot_and_delayed_ack),
        cmocka_unit_test(test_online_learner_requires_reconciliation_after_leader_change),
        cmocka_unit_test(test_online_learner_staging_rejects_invalid_calls),
        cmocka_unit_test(test_recovered_hard_state_must_match_fixed_voters),
#ifdef __linux__
        cmocka_unit_test(test_legacy_recovered_vote_outside_voters_preserves_storage),
        cmocka_unit_test(test_durable_voter_binding_is_atomic_before_first_vote),
        cmocka_unit_test(test_durable_learner_snapshot_suffix_and_role_survive_restart),
        cmocka_unit_test(test_durable_v4_corrupt_commit_or_roles_fail_without_rewriting),
        cmocka_unit_test(test_durable_snapshot_adoption_preserves_vote_log_and_application),
        cmocka_unit_test(test_trusted_legacy_voter_baseline_migrates_durable_state),
        cmocka_unit_test(test_trusted_legacy_voter_baseline_refuses_configuration_logs),
        cmocka_unit_test(test_trusted_legacy_voter_baseline_must_precede_raft_startup),
        cmocka_unit_test(test_trusted_legacy_voter_binding_is_atomic_at_each_publish_stage),
        cmocka_unit_test(test_legacy_disk_backend_cannot_enable_learner_roles),
        cmocka_unit_test(test_legacy_leader_cannot_stage_online_learner),
        cmocka_unit_test(test_online_learner_staging_does_not_change_durable_binding),
#endif
    };
    return cmocka_run_group_tests(tests, nullptr, nullptr);
}
