#include <miniraft/raft.h>
#include <deque>
#include <functional>
#include <set>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
extern "C" {
#include <cmocka.h>
}

namespace {
struct TEndpoint : INode {
    std::function<void(TMessageHolder<TMessage>)> Deliver;
    explicit TEndpoint(std::function<void(TMessageHolder<TMessage>)> f) : Deliver(std::move(f)) {}
    void Send(TMessageHolder<TMessage> m) override { Deliver(std::move(m)); }
    void Drain() override {}
};

// Virtual time and a manually scheduled network: no sockets, sleeps or wall clock.
struct TCluster {
    ITimeSource::Time Now{};
    std::vector<std::shared_ptr<TRaft>> Nodes;
    std::deque<TMessageHolder<TMessage>> Packets;
    std::set<uint32_t> Isolated;
    explicit TCluster(uint32_t count) {
        for (uint32_t id = 1; id <= count; ++id) {
            TNodeDict peers;
            for (uint32_t dst = 1; dst <= count; ++dst) {
                if (dst != id) peers[dst] = std::make_shared<TEndpoint>([this](auto m) {
                    Packets.push_back(std::move(m));
                });
            }
            Nodes.push_back(std::make_shared<TRaft>(std::make_shared<TState>(), id, peers));
        }
    }
    void Drain(bool reverse = false, bool duplicate = false) {
        size_t budget = 10000;
        while (!Packets.empty()) {
            assert_true(budget-- > 0);
            auto m = reverse ? Packets.back() : Packets.front();
            if (reverse) Packets.pop_back(); else Packets.pop_front();
            auto ex = m.Cast<TMessageEx>();
            if (Isolated.count(ex->Src) || Isolated.count(ex->Dst)) continue;
            Nodes.at(ex->Dst - 1)->Process(Now, m);
            if (duplicate) Nodes.at(ex->Dst - 1)->Process(Now, m);
        }
    }
    void Elect(uint32_t id) {
        Now += std::chrono::seconds(10);
        auto& node = Nodes.at(id - 1);
        node->ProcessTimeout(Now);
        Drain();
        node->ProcessTimeout(Now);
        Drain();
        assert_int_equal(node->CurrentStateName(), EState::LEADER);
    }
    void Replicate(uint32_t id, bool reverse = false, bool duplicate = false) {
        for (int round = 0; round < 12; ++round) {
            Now += std::chrono::milliseconds(500);
            Nodes.at(id - 1)->ProcessTimeout(Now);
            Drain(reverse, duplicate);
        }
    }
    void AssertCommittedPrefix() {
        for (auto& a : Nodes) for (auto& b : Nodes) {
            auto end = std::min(a->GetVolatileState()->CommitIndex, b->GetVolatileState()->CommitIndex);
            for (uint64_t i = 1; i <= end; ++i) {
                auto x = a->GetState()->Get(i), y = b->GetState()->Get(i);
                assert_int_equal(x->Len, y->Len);
                assert_memory_equal(x.Mes, y.Mes, x->Len);
            }
        }
    }
};

void test_majority_all_cluster_sizes(void**) {
    for (int count = 1; count <= 8; ++count) {
        TState state;
        state.Append(NewHoldedMessage<TLogEntry>());
        for (int copies = 1; copies <= count; ++copies) {
            TVolatileState v;
            for (int peer = 1; peer < copies; ++peer) v.MatchIndex[peer + 1] = 1;
            v.CommitAdvance(count, state);
            assert_int_equal(v.CommitIndex, copies >= count / 2 + 1 ? 1 : 0);
        }
    }
}

void CheckRequests(uint32_t flags, bool partial) {
    auto state = std::make_shared<TState>();
    auto raft = std::make_shared<TRaft>(state, 1, TNodeDict{});
    raft->Become(EState::LEADER);
    auto rsm = std::make_shared<TDummyRsm>();
    TRequestProcessor processor(raft, rsm, {});
    std::vector<uint32_t> replies;
    auto client = std::make_shared<TEndpoint>([&](auto m) {
        replies.push_back(m.template Cast<TCommandResponse>()->Cookie);
    });
    for (uint32_t cookie : {11, 22}) {
        auto command = NewHoldedMessage<TCommandRequest>(sizeof(TCommandRequest) + sizeof(int64_t));
        int64_t readIndex = 0;
        memcpy(command->Data, &readIndex, sizeof(readIndex));
        command->Flags = flags;
        command->Cookie = cookie;
        processor.OnCommandRequest(command, client);
    }
    if (partial) {
        // Only the first write is committed; a later request must not block it.
        const_cast<TVolatileState*>(raft->GetVolatileState())->CommitIndex = 1;
        processor.ProcessCommitted();
        processor.ProcessWaiting();
        assert_int_equal(replies.size(), 1);
        assert_int_equal(replies[0], 11);
    }
    raft->ProcessTimeout(ITimeSource::Time{});
    processor.ProcessCommitted();
    processor.ProcessWaiting();
    assert_int_equal(replies.size(), 2);
    assert_int_equal(replies[0], 11);
    assert_int_equal(replies[1], 22);
    processor.ProcessWaiting();
    assert_int_equal(replies.size(), 2);
}
void test_read_fifo(void**) { CheckRequests(0, false); }
void test_strong_read_fifo(void**) { CheckRequests(TCommandRequest::EConsistent, false); }
void test_write_fifo(void**) { CheckRequests(TCommandRequest::EWrite, false); }
void test_partial_write_completion(void**) { CheckRequests(TCommandRequest::EWrite, true); }

void test_loss_reorder_duplicate_and_recovery(void**) {
    TCluster cluster(3);
    cluster.Elect(1);
    auto index = cluster.Nodes[0]->Append(NewHoldedMessage<TLogEntry>());
    cluster.Nodes[0]->ProcessTimeout(cluster.Now += std::chrono::seconds(1));
    cluster.Packets.clear(); // Drop a complete replication round.
    assert_true(cluster.Nodes[0]->GetVolatileState()->CommitIndex < index);
    cluster.Replicate(1, true, true);
    for (auto& node : cluster.Nodes) assert_int_equal(node->GetVolatileState()->CommitIndex, index);
    cluster.AssertCommittedPrefix();
}

void test_partition_leader_replacement_and_heal(void**) {
    TCluster cluster(3);
    cluster.Elect(1);
    auto committed = cluster.Nodes[0]->Append(NewHoldedMessage<TLogEntry>());
    cluster.Replicate(1);
    cluster.Nodes[0]->ProcessTimeout(cluster.Now += std::chrono::seconds(1));
    auto delayed = std::move(cluster.Packets);
    cluster.Packets.clear();
    cluster.Isolated.insert(1);
    auto lost = NewHoldedMessage<TLogEntry>();
    lost->Flags = 8; // Distinguish the minority's uncommitted entry.
    auto minority = cluster.Nodes[0]->Append(lost);
    cluster.Replicate(1);
    assert_int_equal(cluster.Nodes[0]->GetVolatileState()->CommitIndex, committed);
    cluster.Elect(2);
    auto replacement = cluster.Nodes[1]->Append(NewHoldedMessage<TLogEntry>());
    cluster.Replicate(2);
    assert_true(cluster.Nodes[1]->GetVolatileState()->CommitIndex >= replacement);
    cluster.AssertCommittedPrefix();
    cluster.Isolated.clear();
    // Old-term traffic arrives after the new leader has committed its entry.
    cluster.Packets = std::move(delayed);
    cluster.Drain(true, true);
    cluster.Replicate(2, true, true);
    assert_int_equal(cluster.Nodes[0]->CurrentStateName(), EState::FOLLOWER);
    for (auto& node : cluster.Nodes) {
        assert_int_equal(node->GetVolatileState()->CommitIndex, replacement);
        assert_int_equal(node->GetState()->Get(minority)->Flags, TLogEntry::EStub);
    }
    cluster.AssertCommittedPrefix();
}

void test_even_cluster_partition_requires_majority(void**) {
    TCluster cluster(4);
    cluster.Elect(1);
    cluster.Replicate(1);
    auto previous = cluster.Nodes[0]->GetVolatileState()->CommitIndex;
    cluster.Isolated = {3, 4};
    auto index = cluster.Nodes[0]->Append(NewHoldedMessage<TLogEntry>());
    cluster.Replicate(1);
    assert_int_equal(cluster.Nodes[0]->GetVolatileState()->CommitIndex, previous);
    assert_int_equal(cluster.Nodes[1]->GetVolatileState()->CommitIndex, previous);
    cluster.Isolated.clear();
    cluster.Replicate(1, true, true);
    for (auto& node : cluster.Nodes) assert_int_equal(node->GetVolatileState()->CommitIndex, index);
    cluster.AssertCommittedPrefix();
}
}

int main() {
    const CMUnitTest tests[] = {
        cmocka_unit_test(test_majority_all_cluster_sizes),
        cmocka_unit_test(test_read_fifo),
        cmocka_unit_test(test_strong_read_fifo),
        cmocka_unit_test(test_write_fifo),
        cmocka_unit_test(test_partial_write_completion),
        cmocka_unit_test(test_loss_reorder_duplicate_and_recovery),
        cmocka_unit_test(test_partition_leader_replacement_and_heal),
        cmocka_unit_test(test_even_cluster_partition_requires_majority),
    };
    return cmocka_run_group_tests(tests, nullptr, nullptr);
}
