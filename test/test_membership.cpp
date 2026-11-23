#include <miniraft/raft.h>
#include <miniraft/persist.h>
#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <gtest/gtest.h>

namespace {

using namespace std::chrono_literals;

struct TEndpoint : INode {
    std::function<void(TMessageHolder<TMessage>)> Deliver;
    explicit TEndpoint(std::function<void(TMessageHolder<TMessage>)> f) : Deliver(std::move(f)) {}
    void Send(TMessageHolder<TMessage> m) override { Deliver(std::move(m)); }
    void Drain() override {}
};

// Virtual time and a manually scheduled network; every node runs its timers.
struct TCluster {
    ITimeSource::Time Now{};
    std::vector<std::shared_ptr<TRaft>> Nodes;
    std::vector<TConfiguration> Initial;
    std::deque<TMessageHolder<TMessage>> Packets;
    // Returns true to lose a packet.
    std::function<bool(const TMessageHolder<TMessageEx>&)> Drop;

    // Without a configuration all nodes are voters. With one, its members start with it,
    // and the other nodes join with an empty one.
    explicit TCluster(uint32_t count, std::optional<TConfiguration> configuration = {}) {
        for (uint32_t id = 1; id <= count; ++id) {
            TConfiguration initial;
            if (!configuration) {
                for (uint32_t voter = 1; voter <= count; ++voter) {
                    initial.Voters.insert(voter);
                }
            } else if (configuration->Voters.contains(id) || configuration->Learners.contains(id)) {
                initial = *configuration;
            }
            Initial.push_back(initial);
        }
        Nodes.resize(count);
        for (uint32_t id = 1; id <= count; ++id) {
            Start(id, std::make_shared<TState>());
        }
    }
    void Start(uint32_t id, std::shared_ptr<IState> state) {
        TNodeDict peers;
        for (uint32_t dst = 1; dst <= Initial.size(); ++dst) {
            if (dst != id) {
                peers[dst] = std::make_shared<TEndpoint>([this](auto m) { Packets.push_back(std::move(m)); });
            }
        }
        Nodes.at(id - 1) = std::make_shared<TRaft>(std::move(state), id, peers, Initial.at(id - 1));
    }
    // Keeps the persistent state only.
    void Restart(uint32_t id) {
        Start(id, (*this)[id].GetState());
    }
    TRaft& operator[](uint32_t id) {
        return *Nodes.at(id - 1);
    }
    uint64_t Term(uint32_t id) {
        return (*this)[id].GetState()->CurrentTerm;
    }
    uint64_t Commit(uint32_t id) {
        return (*this)[id].GetVolatileState()->CommitIndex;
    }
    void Write(uint32_t leader, int count = 1) {
        for (int i = 0; i < count; ++i) {
            (*this)[leader].Append(NewHoldedMessage<TLogEntry>());
        }
    }
    void Replicate(int steps = 12) {
        for (int i = 0; i < steps; ++i) {
            ASSERT_NO_FATAL_FAILURE(Step());
        }
    }
    void Drain() {
        size_t budget = 100000;
        while (!Packets.empty()) {
            ASSERT_TRUE(budget-- > 0);
            auto m = Packets.front().Cast<TMessageEx>();
            Packets.pop_front();
            if (!Drop || !Drop(m)) {
                (*this)[m->Dst].Process(Now, m);
            }
        }
    }
    void Step(std::chrono::milliseconds dt = 500ms) {
        Now += dt;
        for (auto& node : Nodes) {
            node->ProcessTimeout(Now);
        }
        ASSERT_NO_FATAL_FAILURE(Drain());
    }
    // A snapshot of the committed log with no application records.
    void Compact(uint32_t id, uint64_t index = 0) {
        auto& node = (*this)[id];
        node.Compact(NewHoldedMessage(TMessageEx{}, TInstallSnapshotRequest{
            .LastIncludedIndex = index ? index : node.GetVolatileState()->CommitIndex}));
    }
    template<class F>
    void StepUntil(F done, int steps = 100) {
        for (int i = 0; i < steps && !done(); ++i) {
            ASSERT_NO_FATAL_FAILURE(Step());
        }
        ASSERT_TRUE(done());
    }
    // Configuration entries in the log of a node above an index.
    int Changes(uint32_t id, uint64_t above) {
        int changes = 0;
        const auto& state = (*this)[id].GetState();
        for (auto i = std::max(above, state->SnapshotIndex) + 1; i <= state->LastLogIndex; ++i) {
            changes += state->Get(i)->Flags == TLogEntry::EConfiguration;
        }
        return changes;
    }
    void AssertCommittedPrefix() {
        for (auto& a : Nodes) {
            for (auto& b : Nodes) {
                auto end = std::min(a->GetVolatileState()->CommitIndex, b->GetVolatileState()->CommitIndex);
                for (uint64_t i = 1; i <= end; ++i) {
                    auto x = a->GetState()->Get(i);
                    auto y = b->GetState()->Get(i);
                    ASSERT_EQ(x->Len, y->Len);
                    ASSERT_EQ(0, memcmp(x.Mes, y.Mes, x->Len)) << "index " << i;
                }
            }
        }
    }
    void Elect(uint32_t id) {
        Now += 10s;
        (*this)[id].ProcessTimeout(Now);
        ASSERT_NO_FATAL_FAILURE(Drain());
        (*this)[id].ProcessTimeout(Now);
        ASSERT_NO_FATAL_FAILURE(Drain());
        ASSERT_EQ((*this)[id].CurrentStateName(), EState::LEADER);
    }
};

TEST(Membership, server_without_heartbeats_cannot_depose_the_leader) {
    TCluster cluster(3);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    ASSERT_NO_FATAL_FAILURE(cluster.Step());
    const auto term = cluster.Term(1);
    // Node 3 hears nothing, like a removed server: it campaigns with ever higher terms.
    cluster.Drop = [](const auto& m) { return m->Dst == 3; };
    for (int i = 0; i < 100; ++i) {
        ASSERT_NO_FATAL_FAILURE(cluster.Step());
    }
    ASSERT_GT(cluster.Term(3), term + 2);
    ASSERT_EQ(cluster[1].CurrentStateName(), EState::LEADER);
    ASSERT_EQ(cluster.Term(1), term);
    ASSERT_EQ(cluster.Term(2), term);

    // Once the leader is gone its heartbeats stop holding the votes back.
    cluster.Drop = [](const auto& m) { return m->Src == 1 || m->Dst == 1; };
    auto elected = [&] {
        return cluster[2].CurrentStateName() == EState::LEADER || cluster[3].CurrentStateName() == EState::LEADER;
    };
    for (int i = 0; i < 100 && !elected(); ++i) {
        ASSERT_NO_FATAL_FAILURE(cluster.Step());
    }
    ASSERT_TRUE(elected());
    ASSERT_GT(cluster.Term(2), term);
}

// With links 2->1 and 1->3 down, node 2 hears the leader, which hears nobody, and node 3
// hears nobody. The leader must step down, or node 2 would hold its vote back forever.
TEST(Membership, leader_that_hears_no_majority_steps_down) {
    TCluster cluster(3);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    ASSERT_NO_FATAL_FAILURE(cluster.Step());
    cluster.Drop = [](const auto& m) {
        return (m->Src == 2 && m->Dst == 1) || (m->Src == 1 && m->Dst == 3);
    };
    auto leader = [&]() -> uint32_t {
        for (uint32_t id : {2, 3}) {
            if (cluster[id].CurrentStateName() == EState::LEADER) {
                return id;
            }
        }
        return 0;
    };
    for (int i = 0; i < 400 && !leader(); ++i) {
        ASSERT_NO_FATAL_FAILURE(cluster.Step());
    }
    const auto next = leader();
    ASSERT_NE(next, 0);
    ASSERT_NE(cluster[1].CurrentStateName(), EState::LEADER);
    // Nodes 2 and 3 hear each other: the new leader stays and commits.
    cluster[next].Append(NewHoldedMessage<TLogEntry>());
    for (int i = 0; i < 100; ++i) {
        ASSERT_NO_FATAL_FAILURE(cluster.Step());
    }
    ASSERT_EQ(cluster[next].CurrentStateName(), EState::LEADER);
    ASSERT_EQ(cluster[next].GetVolatileState()->CommitIndex, cluster[next].GetLastIndex());
}

TEST(Membership, learners_get_the_log_but_never_count) {
    TCluster cluster(3, TConfiguration{.Voters = {1, 2}, .Learners = {3}});
    ASSERT_EQ(cluster[3].GetNservers(), 2);
    ASSERT_EQ(cluster[3].GetNpeers(), 2);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    cluster.Write(1, 3);
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate());
    const auto last = cluster[1].GetLastIndex();
    ASSERT_EQ(cluster[3].GetLastIndex(), last);
    ASSERT_EQ(cluster.Commit(3), last);

    // The learner's copy does not commit an entry, nor does its reply confirm a read.
    cluster.Drop = [](const auto& m) { return m->Src == 2 || m->Dst == 2; };
    cluster.Write(1);
    const auto read = cluster[1].ApproveRead();
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate(4));
    ASSERT_EQ(cluster[1].GetVolatileState()->MatchIndex.at(3), last + 1);
    ASSERT_GT(cluster[1].GetVolatileState()->ReadAckSeqno.at(3), read);
    ASSERT_EQ(cluster.Commit(1), last);
    ASSERT_LT(cluster[1].CommitSeqno(), read);
    // The voter's do.
    cluster.Drop = [](const auto& m) { return m->Src == 3 || m->Dst == 3; };
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate(4));
    ASSERT_EQ(cluster.Commit(1), last + 1);
    ASSERT_GE(cluster[1].CommitSeqno(), read);

    // Cut off from the leader, the learner never campaigns.
    const auto term = cluster.Term(3);
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate(100));
    ASSERT_EQ(cluster.Term(3), term);
    ASSERT_EQ(cluster[3].CurrentStateName(), EState::FOLLOWER);
    ASSERT_EQ(cluster[1].CurrentStateName(), EState::LEADER);

    // Nor do its replies keep the leader without the voter.
    cluster.Drop = [](const auto& m) { return m->Src == 2 || m->Dst == 2; };
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate(30));
    ASSERT_NE(cluster[1].CurrentStateName(), EState::LEADER);
}

TEST(Membership, configuration_is_checked) {
    TNodeDict peers{{2, std::make_shared<TEndpoint>([](auto) {})}};
    auto state = std::make_shared<TState>();
    for (auto configuration : {TConfiguration{.Learners = {1, 2}},
        TConfiguration{.Voters = {1, 2}, .Learners = {2}}, TConfiguration{.Voters = {1, 3}}})
    {
        ASSERT_THROW(TRaft(state, 1, peers, configuration), std::invalid_argument);
    }
    TRaft learner(state, 1, peers, TConfiguration{.Voters = {2}, .Learners = {1}});
    ASSERT_EQ(learner.GetConfiguration(), (TConfiguration{.Voters = {2}, .Learners = {1}}));
    TRaft voter(state, 1, peers);
    ASSERT_EQ(voter.GetConfiguration(), (TConfiguration{.Voters = {1, 2}}));
    // A server that joins a running cluster learns its configuration from the leader.
    TRaft joiner(state, 1, peers, TConfiguration{});
    ASSERT_EQ(joiner.GetConfiguration(), TConfiguration{});
}

TEST(Membership, learner_vote_does_not_count) {
    TCluster cluster(4, TConfiguration{.Voters = {1, 2, 3}, .Learners = {4}});
    cluster.Drop = [](const auto& m) { return m->Dst != 1; };
    cluster.Now += 10s;
    auto& candidate = cluster[1];
    candidate.ProcessTimeout(cluster.Now);
    ASSERT_EQ(candidate.CurrentStateName(), EState::CANDIDATE);
    ASSERT_NO_FATAL_FAILURE(cluster.Drain());
    candidate.Process(cluster.Now, NewHoldedMessage(
        TMessageEx{.Src = 4, .Dst = 1, .Term = cluster.Term(1)},
        TRequestVoteResponse{.VoteGranted = 1}));
    candidate.ProcessTimeout(cluster.Now);
    ASSERT_EQ(candidate.CurrentStateName(), EState::CANDIDATE);
    candidate.Process(cluster.Now, NewHoldedMessage(
        TMessageEx{.Src = 3, .Dst = 1, .Term = cluster.Term(1)},
        TRequestVoteResponse{.VoteGranted = 1}));
    candidate.ProcessTimeout(cluster.Now);
    ASSERT_EQ(candidate.CurrentStateName(), EState::LEADER);
}

TEST(Membership, learner_catches_up_in_rounds_shorter_than_an_election_timeout) {
    TCluster cluster(4, TConfiguration{.Voters = {1, 2, 3}, .Learners = {4}});
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    const auto& rounds = cluster[1].GetVolatileState()->CatchUp;
    auto caughtUp = [&] { return rounds.at(4).CaughtUp(cluster.Now); };
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate());
    ASSERT_TRUE(caughtUp());
    // Current at any time, not only at the leader's timeouts.
    ASSERT_FALSE(rounds.at(4).CaughtUp(cluster.Now + TTimeout::Election));

    // A round that runs longer than an election timeout stops counting.
    cluster.Drop = [](const auto& m) { return m->Dst == 4; };
    cluster.Write(1, 50);
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate());
    ASSERT_FALSE(caughtUp());
    cluster.Drop = {};
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate());
    ASSERT_TRUE(caughtUp());

    // Over a slow link every round takes longer than an election timeout.
    int requests = 0;
    cluster.Drop = [&](const auto& m) {
        return m->Dst == 4 && m->Type == uint32_t(EMessageType::APPEND_ENTRIES_REQUEST) && ++requests % 4;
    };
    for (int i = 0; i < 80; ++i) {
        cluster.Write(1);
        ASSERT_NO_FATAL_FAILURE(cluster.Step());
        if (i >= 20) {
            ASSERT_FALSE(caughtUp()) << i;
        }
    }
    cluster.Drop = {};
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate());
    ASSERT_TRUE(caughtUp());
    ASSERT_EQ(cluster[1].GetVolatileState()->MatchIndex.at(4), cluster[1].GetLastIndex());
}

// Counts the entries the application gets.
struct TCountingRsm : IRsm {
    int Writes = 0;
    TMessageHolder<TMessage> Read(TMessageHolder<TCommandRequest>, uint64_t index) override {
        return NewHoldedMessage(TCommandResponse{.Index = index});
    }
    TMessageHolder<TMessage> Write(TMessageHolder<TLogEntry> entry, uint64_t index) override {
        ++Writes;
        LastAppliedIndex = index;
        return {};
    }
    TMessageHolder<TLogEntry> Prepare(TMessageHolder<TCommandRequest>) override {
        return NewHoldedMessage<TLogEntry>();
    }
    TMessageHolder<TInstallSnapshotRequest> Snapshot() const override {
        throw std::runtime_error("Not implemented");
    }
    void Apply(TMessageHolder<TInstallSnapshotRequest>) override {
        throw std::runtime_error("Not implemented");
    }
};

TEST(Membership, add_a_voter_through_a_learner) {
    TCluster cluster(4, TConfiguration{.Voters = {1, 2, 3}});
    ASSERT_EQ(cluster[4].GetConfiguration(), TConfiguration{});
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    auto& leader = cluster[1];
    cluster.Write(1, 3);
    const auto learner = leader.AddLearner(4);
    ASSERT_EQ(learner, leader.GetLastIndex());
    const TConfiguration withLearner{.Index = learner, .Voters = {1, 2, 3}, .Learners = {4}};
    ASSERT_EQ(leader.GetConfiguration(), withLearner);
    // One change at a time, and no promotion before the learner has caught up.
    ASSERT_EQ(leader.RemoveServer(3), 0);
    cluster.Drop = [](const auto& m) { return m->Dst == 4; };
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate());
    ASSERT_GE(cluster.Commit(1), learner);
    ASSERT_EQ(leader.PromoteLearner(cluster.Now, 4), 0);
    cluster.Drop = {};
    uint64_t voter = 0;
    ASSERT_NO_FATAL_FAILURE(cluster.StepUntil([&] { return voter || (voter = leader.PromoteLearner(cluster.Now, 4)); }));
    ASSERT_EQ(cluster[4].GetConfiguration(), withLearner);
    ASSERT_EQ(cluster[4].GetLastIndex(), voter - 1);
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate());
    for (uint32_t id = 1; id <= 4; ++id) {
        ASSERT_EQ(cluster[id].GetConfiguration(), (TConfiguration{.Index = voter, .Voters = {1, 2, 3, 4}}));
        ASSERT_GE(cluster.Commit(id), voter);
    }
    // Four voters: an entry needs three copies.
    cluster.Drop = [](const auto& m) { return m->Dst == 3 || m->Dst == 4; };
    cluster.Write(1);
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate(4));
    ASSERT_LT(cluster.Commit(1), leader.GetLastIndex());
    cluster.Drop = [](const auto& m) { return m->Dst == 3; };
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate(4));
    ASSERT_EQ(cluster.Commit(1), leader.GetLastIndex());

    // The new voter can lead, and the application never sees a configuration entry.
    cluster.Drop = {};
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(4));
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate());
    ASSERT_EQ(cluster.Commit(4), cluster[4].GetLastIndex());
    auto rsm = std::make_shared<TCountingRsm>();
    TRequestProcessor processor(cluster.Nodes[3], rsm, {});
    processor.ProcessCommitted();
    ASSERT_EQ(rsm->Writes, 4);
}

TEST(Membership, removed_voter_cannot_disrupt_the_cluster) {
    TCluster cluster(3);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate());
    const auto index = cluster[1].RemoveServer(3);
    ASSERT_NE(index, 0);
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate());
    for (uint32_t id : {1, 2}) {
        ASSERT_EQ(cluster[id].GetConfiguration(), (TConfiguration{.Index = index, .Voters = {1, 2}}));
    }
    ASSERT_GE(cluster.Commit(1), index);
    // The leader stops replicating to node 3 with the entry that removes it.
    ASSERT_LT(cluster[3].GetLastIndex(), index);
    ASSERT_FALSE(cluster[1].GetVolatileState()->MatchIndex.contains(3));

    // Node 3 does not know it was removed: it campaigns with ever higher terms.
    const auto term = cluster.Term(1);
    for (int i = 0; i < 100; ++i) {
        cluster.Write(1);
        ASSERT_NO_FATAL_FAILURE(cluster.Step());
    }
    ASSERT_GT(cluster.Term(3), term + 2);
    ASSERT_EQ(cluster[1].CurrentStateName(), EState::LEADER);
    ASSERT_EQ(cluster.Term(1), term);
    ASSERT_EQ(cluster.Term(2), term);
    ASSERT_EQ(cluster.Commit(1), cluster[1].GetLastIndex());
}

TEST(Membership, removed_leader_steps_down_once_the_change_commits) {
    TCluster cluster(3);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate());
    auto& leader = cluster[1];
    // The leader does not count itself: without node 3 the change cannot commit.
    cluster.Drop = [](const auto& m) { return m->Src == 3 || m->Dst == 3; };
    const auto index = leader.RemoveServer(1);
    ASSERT_NE(index, 0);
    cluster.Write(1);
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate(4));
    ASSERT_EQ(leader.CurrentStateName(), EState::LEADER);
    ASSERT_EQ(leader.GetVolatileState()->MatchIndex.at(2), leader.GetLastIndex());
    ASSERT_LT(cluster.Commit(1), index);

    cluster.Drop = {};
    ASSERT_NO_FATAL_FAILURE(cluster.StepUntil([&] { return leader.CurrentStateName() != EState::LEADER; }));
    ASSERT_EQ(cluster.Commit(1), leader.GetLastIndex());
    ASSERT_EQ(leader.CurrentStateName(), EState::FOLLOWER);

    // The rest elect a leader among themselves; node 1 never campaigns again.
    const auto term = cluster.Term(1);
    auto elected = [&] {
        return cluster[2].CurrentStateName() == EState::LEADER || cluster[3].CurrentStateName() == EState::LEADER;
    };
    ASSERT_NO_FATAL_FAILURE(cluster.StepUntil(elected));
    const uint32_t next = cluster[2].CurrentStateName() == EState::LEADER ? 2 : 3;
    cluster.Write(next);
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate(100));
    ASSERT_EQ(cluster.Term(1), term);
    ASSERT_EQ(leader.CurrentStateName(), EState::FOLLOWER);
    ASSERT_EQ(cluster.Commit(next), cluster[next].GetLastIndex());
    ASSERT_EQ(cluster[next].GetConfiguration(), (TConfiguration{.Index = index, .Voters = {2, 3}}));
}

// Thesis, 4.2.2: a server outside its configuration may be the only one that can commit it.
TEST(Membership, removed_leader_that_restarts_before_the_change_commits_leads_again) {
    TCluster cluster(2);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate());
    cluster.Drop = [](const auto& m) { return m->Src == 1; };
    const auto index = cluster[1].RemoveServer(1);
    ASSERT_NE(index, 0);
    ASSERT_NO_FATAL_FAILURE(cluster.Step());
    cluster.Restart(1);
    ASSERT_EQ(cluster[1].GetConfiguration(), (TConfiguration{.Index = index, .Voters = {2}}));
    // Node 1 campaigns but does not count its own vote.
    cluster.Drop = [](const auto& m) { return m->Dst == 2; };
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate(30));
    ASSERT_EQ(cluster[1].CurrentStateName(), EState::CANDIDATE);
    // Node 2 needs the vote of node 1, which has the longer log. Node 1 wins with the
    // vote of node 2 alone, commits the change and steps down.
    cluster.Drop = {};
    ASSERT_NO_FATAL_FAILURE(cluster.StepUntil([&] {
        return cluster[2].CurrentStateName() == EState::LEADER && cluster.Commit(2) >= index;
    }));
    ASSERT_EQ(cluster[2].GetConfiguration(), (TConfiguration{.Index = index, .Voters = {2}}));
    ASSERT_EQ(cluster[1].CurrentStateName(), EState::FOLLOWER);
    ASSERT_NO_FATAL_FAILURE(cluster.AssertCommittedPrefix());
}

TEST(Membership, restart_and_truncation_in_the_middle_of_a_change) {
    TCluster cluster(4, TConfiguration{.Voters = {1, 2, 3}});
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate());
    // Only the leader gets the entry before it crashes.
    cluster.Drop = [](const auto& m) { return m->Src == 1 || m->Dst == 1; };
    const auto index = cluster[1].AddLearner(4);
    ASSERT_NE(index, 0);
    ASSERT_NO_FATAL_FAILURE(cluster.Step());
    // A restart recovers the configuration from the log, committed or not.
    cluster.Restart(1);
    const TConfiguration added{.Index = index, .Voters = {1, 2, 3}, .Learners = {4}};
    ASSERT_EQ(cluster[1].GetConfiguration(), added);

    // The others elect a leader that does not have the entry. Once it overwrites
    // the entry, node 1 falls back to the configuration before it.
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(2));
    cluster.Drop = {};
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate());
    ASSERT_EQ(cluster[2].CurrentStateName(), EState::LEADER);
    ASSERT_EQ(cluster[1].GetConfiguration(), (TConfiguration{.Voters = {1, 2, 3}}));
    ASSERT_EQ(cluster[1].GetLastIndex(), cluster[2].GetLastIndex());

    // The new leader makes the change; a restarted follower recovers it.
    const auto again = cluster[2].AddLearner(4);
    ASSERT_NE(again, 0);
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate());
    cluster.Restart(3);
    ASSERT_EQ(cluster[3].GetConfiguration(), (TConfiguration{.Index = again, .Voters = {1, 2, 3}, .Learners = {4}}));
    ASSERT_NO_FATAL_FAILURE(cluster.AssertCommittedPrefix());
}

TEST(Membership, change_with_a_lagging_voter) {
    TCluster cluster(4, TConfiguration{.Voters = {1, 2, 3}});
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate());
    cluster.Drop = [](const auto& m) { return m->Src == 3 || m->Dst == 3; };
    cluster.Write(1, 5);
    auto& leader = cluster[1];
    ASSERT_NE(leader.AddLearner(4), 0);
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate());
    const auto voter = leader.PromoteLearner(cluster.Now, 4);
    ASSERT_NE(voter, 0);
    // Nodes 1, 2 and 4 are a majority of the four voters.
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate());
    ASSERT_GE(cluster.Commit(1), voter);
    ASSERT_LT(cluster[3].GetLastIndex(), voter);

    // Its newer term may cost an election, but it gets both configurations.
    cluster.Drop = {};
    ASSERT_NO_FATAL_FAILURE(cluster.StepUntil([&] {
        return cluster[3].GetConfiguration().Index == voter && cluster.Commit(3) >= voter;
    }, 200));
    ASSERT_EQ(cluster[3].GetConfiguration(), (TConfiguration{.Index = voter, .Voters = {1, 2, 3, 4}}));
    ASSERT_NO_FATAL_FAILURE(cluster.AssertCommittedPrefix());
}

// The scenario of the fix (Ongaro, raft-dev, 2015-07-10). Without it the new leader
// would commit a configuration that has no majority in common with the one of node 1.
TEST(Membership, new_leader_changes_the_configuration_after_an_entry_of_its_term) {
    TCluster cluster(5, TConfiguration{.Voters = {1, 2, 3, 4}});
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate());
    ASSERT_NE(cluster[1].AddLearner(5), 0);
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate());
    // Only node 5 gets its promotion: node 1 alone thinks there are five voters.
    cluster.Drop = [](const auto& m) { return m->Src == 1 && m->Dst != 5; };
    const auto promoted = cluster[1].PromoteLearner(cluster.Now, 5);
    ASSERT_NE(promoted, 0);
    ASSERT_NO_FATAL_FAILURE(cluster.Step());
    ASSERT_EQ(cluster[5].GetConfiguration().Index, promoted);

    // Node 2 wins with the votes of 3 and 4, but only node 3 gets its entries.
    cluster.Drop = [](const auto& m) { return m->Src == 1 || m->Dst == 1; };
    cluster.Now += 10s;
    cluster[2].ProcessTimeout(cluster.Now);
    ASSERT_NO_FATAL_FAILURE(cluster.Drain());
    cluster.Drop = [](const auto& m) {
        return m->Src == 1 || m->Dst == 1 || m->Dst == 4 || m->Dst == 5;
    };
    cluster[2].ProcessTimeout(cluster.Now);
    ASSERT_EQ(cluster[2].CurrentStateName(), EState::LEADER);
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate(4));
    ASSERT_EQ(cluster[3].GetLastIndex(), cluster[2].GetLastIndex());
    // Nodes 2 and 3 would be a majority of {2, 3, 4}, and nodes 1, 4 and 5 one of
    // the five voters of node 1.
    ASSERT_EQ(cluster[2].RemoveServer(1), 0);

    cluster.Drop = [](const auto& m) { return m->Src == 1 || m->Dst == 1 || m->Dst == 5; };
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate(4));
    const auto removed = cluster[2].RemoveServer(1);
    ASSERT_NE(removed, 0);
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate(4));
    ASSERT_GE(cluster.Commit(2), removed);

    // Node 1 comes back and cannot win: node 4 has an entry of the newer term.
    cluster.Drop = {};
    cluster.Write(2);
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate(100));
    ASSERT_EQ(cluster[2].CurrentStateName(), EState::LEADER);
    ASSERT_NE(cluster[1].CurrentStateName(), EState::LEADER);
    ASSERT_EQ(cluster.Commit(2), cluster[2].GetLastIndex());
    ASSERT_NO_FATAL_FAILURE(cluster.AssertCommittedPrefix());
}

TEST(Membership, at_most_one_uncommitted_change_under_failures) {
    TCluster cluster(5, TConfiguration{.Voters = {1, 2, 3}});
    uint32_t seed = 2015;
    auto random = [&] {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return seed;
    };
    int installs = 0;
    cluster.Drop = [&](const auto& m) {
        installs += m->Type == uint32_t(EMessageType::INSTALL_SNAPSHOT_REQUEST);
        return random() % 10 == 0;
    };
    std::map<uint64_t, uint32_t> leaders;
    // Each committed entry as it was first seen committed, and the highest commit index ever.
    std::map<uint64_t, std::string> committed;
    uint64_t highest = 0;
    int changes = 0;
    for (int step = 0; step < 3000; ++step) {
        if (step % 97 == 0) {
            cluster.Restart(1 + random() % 5);
        }
        // The servers that join later catch up through InstallSnapshot.
        if (step % 13 == 0) {
            for (uint32_t id = 1; id <= 3; ++id) {
                ASSERT_NO_FATAL_FAILURE(cluster.Compact(id));
            }
        }
        for (uint32_t id = 1; id <= 5; ++id) {
            auto& node = cluster[id];
            if (node.CurrentStateName() != EState::LEADER) {
                continue;
            }
            auto [leader, _] = leaders.emplace(cluster.Term(id), id);
            ASSERT_EQ(leader->second, id) << "two leaders in term " << leader->first;
            const auto configuration = node.GetConfiguration();
            uint32_t joiner = 0;
            for (uint32_t other = 1; other <= 5; ++other) {
                if (other != id && !configuration.Voters.contains(other) && !configuration.Learners.contains(other)) {
                    joiner = other;
                }
            }
            if (!configuration.Learners.empty()) {
                changes += !!node.PromoteLearner(cluster.Now, *configuration.Learners.begin());
            } else if (joiner && configuration.Voters.size() < 4) {
                changes += !!node.AddLearner(joiner);
            } else if (configuration.Voters.size() > 2) {
                auto voter = configuration.Voters.begin();
                std::advance(voter, random() % configuration.Voters.size());
                changes += !!node.RemoveServer(*voter);
            }
            cluster.Write(id);
        }
        ASSERT_NO_FATAL_FAILURE(cluster.Step());
        // State Machine Safety: a committed entry never changes, on any node.
        for (uint32_t id = 1; id <= 5; ++id) {
            const auto& state = cluster[id].GetState();
            const auto commit = cluster.Commit(id);
            highest = std::max(highest, commit);
            for (auto i = state->SnapshotIndex + 1; i <= commit; ++i) {
                auto entry = state->Get(i);
                std::string bytes(reinterpret_cast<const char*>(entry.Mes), entry->Len);
                auto [first, _] = committed.emplace(i, bytes);
                ASSERT_EQ(first->second, bytes) << "index " << i << ", node " << id << ", step " << step;
            }
        }
        for (uint32_t id = 1; id <= 5; ++id) {
            ASSERT_LE(cluster.Changes(id, highest), 1) << "node " << id << ", step " << step;
        }
    }
    ASSERT_GT(changes, 20);
    ASSERT_GT(leaders.size(), 2);
    ASSERT_GT(installs, 0);
}

TEST(Membership, configuration_survives_compaction_and_restart) {
    struct TDirectory {
        std::filesystem::path Path = std::filesystem::temp_directory_path() /
            ("miniraft-membership-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        TDirectory() { std::filesystem::create_directory(Path); }
        ~TDirectory() { std::error_code ignored; std::filesystem::remove_all(Path, ignored); }
    } directory;
    const auto name = (directory.Path / "raft").string();
    {
        TCluster cluster(4, TConfiguration{.Voters = {1, 2, 3}});
        cluster.Start(2, std::make_shared<TDiskState>(name, 2));
        ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
        ASSERT_NO_FATAL_FAILURE(cluster.Replicate());
        ASSERT_NE(cluster[1].AddLearner(4), 0);
        uint64_t voter = 0;
        ASSERT_NO_FATAL_FAILURE(cluster.StepUntil([&] { return voter || (voter = cluster[1].PromoteLearner(cluster.Now, 4)); }));
        ASSERT_NO_FATAL_FAILURE(cluster.Replicate());
        // The log of node 2 loses both configuration entries.
        ASSERT_NO_FATAL_FAILURE(cluster.Compact(2));
        const auto snapshot = cluster[2].GetState()->SnapshotIndex;
        ASSERT_GE(snapshot, voter);
        const TConfiguration all{.Index = snapshot, .Voters = {1, 2, 3, 4}};
        ASSERT_EQ(cluster[2].GetConfiguration(), all);
        cluster.Write(1, 2);
        ASSERT_NO_FATAL_FAILURE(cluster.Replicate());

        cluster.Start(2, std::make_shared<TDiskState>(name, 2));
        ASSERT_EQ(cluster[2].GetConfiguration(), all);
        // Without node 1, node 2 needs both other voters for a vote and for a commit.
        cluster.Drop = [](const auto& m) { return m->Src == 1 || m->Dst == 1; };
        ASSERT_NO_FATAL_FAILURE(cluster.Elect(2));
        cluster.Write(2);
        ASSERT_NO_FATAL_FAILURE(cluster.Replicate());
        ASSERT_EQ(cluster.Commit(2), cluster[2].GetLastIndex());
    }
}

TEST(Membership, new_server_gets_the_configuration_from_a_snapshot) {
    TCluster cluster(4, TConfiguration{.Voters = {1, 2, 3}});
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate());
    cluster.Drop = [](const auto& m) { return m->Dst == 4; };
    const auto learner = cluster[1].AddLearner(4);
    ASSERT_NE(learner, 0);
    cluster.Write(1, 3);
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate());
    ASSERT_NO_FATAL_FAILURE(cluster.Compact(1));
    const auto snapshot = cluster[1].GetState()->SnapshotIndex;
    ASSERT_GT(snapshot, learner);

    cluster.Drop = {};
    ASSERT_NO_FATAL_FAILURE(cluster.StepUntil([&] { return cluster[4].GetState()->SnapshotIndex == snapshot; }));
    ASSERT_EQ(cluster[4].GetConfiguration(), (TConfiguration{.Index = snapshot, .Voters = {1, 2, 3}, .Learners = {4}}));
    uint64_t voter = 0;
    ASSERT_NO_FATAL_FAILURE(cluster.StepUntil([&] { return voter || (voter = cluster[1].PromoteLearner(cluster.Now, 4)); }));
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate());
    ASSERT_EQ(cluster[4].GetConfiguration(), (TConfiguration{.Index = voter, .Voters = {1, 2, 3, 4}}));
    ASSERT_EQ(cluster.Term(4), cluster.Term(1));
}

TEST(Membership, snapshot_holds_the_configuration_at_its_index) {
    TCluster cluster(4, TConfiguration{.Voters = {1, 2, 3}});
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate());
    const auto learner = cluster[1].AddLearner(4);
    cluster.Write(1, 2);
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate());
    const auto between = cluster.Commit(3);
    ASSERT_GT(between, learner);
    uint64_t voter = 0;
    ASSERT_NO_FATAL_FAILURE(cluster.StepUntil([&] { return voter || (voter = cluster[1].PromoteLearner(cluster.Now, 4)); }));
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate());

    ASSERT_NO_FATAL_FAILURE(cluster.Compact(3, between));
    const TConfiguration all{.Index = voter, .Voters = {1, 2, 3, 4}};
    ASSERT_EQ(cluster[3].GetConfiguration(), all);
    cluster.Restart(3);
    ASSERT_EQ(cluster[3].GetConfiguration(), all);
    // The image alone has the configuration at its index.
    auto image = std::make_shared<TState>();
    image->InstallSnapshot(cluster[3].GetState()->GetSnapshot());
    TRaft restored(image, 3, {}, TConfiguration{});
    ASSERT_EQ(restored.GetConfiguration(), (TConfiguration{.Index = between, .Voters = {1, 2, 3}, .Learners = {4}}));

    // An image made before configurations were in snapshots keeps the initial one.
    auto old = std::make_shared<TState>();
    old->InstallSnapshot(NewHoldedMessage(TMessageEx{}, TInstallSnapshotRequest{
        .LastIncludedIndex = 3, .LastIncludedTerm = 1, .Done = 1}));
    ASSERT_EQ(TRaft(old, 1, {}).GetConfiguration(), (TConfiguration{.Voters = {1}}));
}

TEST(Membership, joiner_does_not_compact_before_it_has_a_configuration) {
    TCluster cluster(4, TConfiguration{.Voters = {1, 2, 3}});
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    cluster.Write(1, 3);
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate());
    ASSERT_NE(cluster[1].AddLearner(4), 0);
    // The entries before the one that adds node 4 come first.
    ASSERT_NO_FATAL_FAILURE(cluster.StepUntil([&] { return cluster.Commit(4) > 0; }));
    ASSERT_EQ(cluster[4].GetConfiguration(), TConfiguration{});
    ASSERT_NO_FATAL_FAILURE(cluster.Compact(4));
    ASSERT_EQ(cluster[4].GetState()->SnapshotIndex, 0);
    cluster.Restart(4);

    ASSERT_NO_FATAL_FAILURE(cluster.Replicate());
    ASSERT_NO_FATAL_FAILURE(cluster.Compact(4));
    const auto snapshot = cluster[4].GetState()->SnapshotIndex;
    ASSERT_GT(snapshot, 0);
    cluster.Restart(4);
    ASSERT_EQ(cluster[4].GetConfiguration(), (TConfiguration{.Index = snapshot, .Voters = {1, 2, 3}, .Learners = {4}}));
}

TEST(Membership, configuration_changes_are_checked) {
    TCluster cluster(3);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate());
    auto& leader = cluster[1];
    ASSERT_THROW(cluster[2].RemoveServer(3), std::logic_error);
    ASSERT_THROW(leader.AddLearner(2), std::invalid_argument);
    ASSERT_THROW(leader.AddLearner(9), std::invalid_argument);
    ASSERT_THROW(leader.PromoteLearner(cluster.Now, 2), std::invalid_argument);
    ASSERT_THROW(leader.RemoveServer(9), std::invalid_argument);
    auto entry = NewHoldedMessage<TLogEntry>();
    entry->Flags = TLogEntry::EConfiguration;
    ASSERT_THROW(leader.Append(entry), TMessageError);

    TCluster single(1);
    ASSERT_NO_FATAL_FAILURE(single.Elect(1));
    ASSERT_NO_FATAL_FAILURE(single.Step());
    ASSERT_THROW(single[1].RemoveServer(1), std::invalid_argument);

    // A malformed configuration entry on the wire is dropped like any invalid RPC.
    const auto last = cluster[2].GetLastIndex();
    for (std::vector<uint32_t> data : {std::vector<uint32_t>{0, 1, 3}, {2, 0, 3, 2}, {1, 1, 2, 2}, {1, 0, 0}, {2, 0, 2}}) {
        auto config = NewHoldedMessage<TLogEntry>(sizeof(TLogEntry) + data.size() * sizeof(uint32_t));
        config->Term = cluster.Term(1);
        config->Flags = TLogEntry::EConfiguration;
        memcpy(config->Data, data.data(), data.size() * sizeof(uint32_t));
        auto append = NewHoldedMessage(
            TMessageEx{.Src = 1, .Dst = 2, .Term = cluster.Term(1), .Seqno = 1000},
            TAppendEntriesRequest{.PrevLogIndex = last, .PrevLogTerm = cluster[2].GetState()->LogTerm(last),
                .LeaderId = 1, .Nentries = 1});
        append.InitPayload(1);
        append.Payload[0] = config;
        cluster[2].Process(cluster.Now, append);
        ASSERT_TRUE(cluster.Packets.empty());
        ASSERT_EQ(cluster[2].GetLastIndex(), last);

        // So is an image that starts with one.
        auto image = NewHoldedMessage(
            TMessageEx{.Src = 1, .Dst = 2, .Term = cluster.Term(1), .Seqno = 1001},
            TInstallSnapshotRequest{.LastIncludedIndex = last + 10, .LastIncludedTerm = cluster.Term(1),
                .LeaderId = 1, .Nentries = 1, .Done = 1});
        image.InitPayload(1);
        image.Payload[0] = config;
        cluster[2].Process(cluster.Now, image);
        ASSERT_TRUE(cluster.Packets.empty());
        ASSERT_EQ(cluster[2].GetState()->SnapshotIndex, 0);
    }
}

} // namespace
