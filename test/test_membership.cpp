#include <miniraft/raft.h>
#include <chrono>
#include <deque>
#include <functional>
#include <optional>
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
    std::deque<TMessageHolder<TMessage>> Packets;
    // Returns true to lose a packet.
    std::function<bool(const TMessageHolder<TMessageEx>&)> Drop;

    // All nodes are voters unless a configuration is given.
    explicit TCluster(uint32_t count, std::optional<TConfiguration> configuration = {}) {
        for (uint32_t id = 1; id <= count; ++id) {
            TNodeDict peers;
            for (uint32_t dst = 1; dst <= count; ++dst) {
                if (dst != id) {
                    peers[dst] = std::make_shared<TEndpoint>([this](auto m) { Packets.push_back(std::move(m)); });
                }
            }
            auto state = std::make_shared<TState>();
            Nodes.push_back(configuration
                ? std::make_shared<TRaft>(state, id, peers, *configuration)
                : std::make_shared<TRaft>(state, id, peers));
        }
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
    for (auto configuration : {TConfiguration{}, TConfiguration{.Learners = {1, 2}},
        TConfiguration{.Voters = {1, 2}, .Learners = {2}}, TConfiguration{.Voters = {1, 3}}})
    {
        ASSERT_THROW(TRaft(state, 1, peers, configuration), std::invalid_argument);
    }
    TRaft learner(state, 1, peers, TConfiguration{.Voters = {2}, .Learners = {1}});
    ASSERT_EQ(learner.GetConfiguration(), (TConfiguration{.Voters = {2}, .Learners = {1}}));
    TRaft voter(state, 1, peers);
    ASSERT_EQ(voter.GetConfiguration(), (TConfiguration{.Voters = {1, 2}}));
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
    auto caughtUp = [&] { return rounds.at(4).CaughtUp; };
    ASSERT_NO_FATAL_FAILURE(cluster.Replicate());
    ASSERT_TRUE(caughtUp());

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

} // namespace
