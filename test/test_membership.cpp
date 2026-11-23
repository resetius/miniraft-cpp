#include <miniraft/raft.h>
#include <chrono>
#include <deque>
#include <functional>
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

    explicit TCluster(uint32_t count) {
        for (uint32_t id = 1; id <= count; ++id) {
            TNodeDict peers;
            for (uint32_t dst = 1; dst <= count; ++dst) {
                if (dst != id) {
                    peers[dst] = std::make_shared<TEndpoint>([this](auto m) { Packets.push_back(std::move(m)); });
                }
            }
            Nodes.push_back(std::make_shared<TRaft>(std::make_shared<TState>(), id, peers));
        }
    }
    TRaft& operator[](uint32_t id) {
        return *Nodes.at(id - 1);
    }
    uint64_t Term(uint32_t id) {
        return (*this)[id].GetState()->CurrentTerm;
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

} // namespace
