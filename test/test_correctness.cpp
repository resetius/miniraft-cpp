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
    void Restart(uint32_t id) {
        auto state = Nodes.at(id - 1)->GetState();
        TNodeDict peers;
        for (uint32_t dst = 1; dst <= Nodes.size(); ++dst) {
            if (dst != id) peers[dst] = std::make_shared<TEndpoint>([this](auto m) {
                Packets.push_back(std::move(m));
            });
        }
        // Retain stable term/vote/log; discard all Raft volatile state.
        Nodes.at(id - 1) = std::make_shared<TRaft>(state, id, peers);
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
    TMessageHolder<TMessage> DeliverOne(uint32_t src, uint32_t dst) {
        for (auto it = Packets.begin(); it != Packets.end(); ++it) {
            auto ex = it->Cast<TMessageEx>();
            if (ex->Src == src && ex->Dst == dst) {
                auto message = *it;
                Packets.erase(it);
                Nodes.at(dst - 1)->Process(Now, message);
                return message;
            }
        }
        fail_msg("Expected a queued packet from %u to %u", src, dst);
        return {};
    }
    void RoundTrip(uint32_t leader, uint32_t peer, bool duplicate = false) {
        DeliverOne(leader, peer);
        auto response = DeliverOne(peer, leader);
        if (duplicate) Nodes.at(leader - 1)->Process(Now, response);
    }
};

struct TReadClient {
    struct TReply { uint32_t Cookie; uint32_t Error; };
    std::vector<TReply> Replies;
    std::shared_ptr<TDummyRsm> Rsm = std::make_shared<TDummyRsm>();
    TRequestProcessor Processor;
    std::shared_ptr<TEndpoint> Endpoint;
    explicit TReadClient(std::shared_ptr<TRaft> raft)
        : Processor(raft, Rsm, {})
        , Endpoint(std::make_shared<TEndpoint>([this](auto message) {
            auto response = message.template Cast<TCommandResponse>();
            Replies.push_back({response->Cookie, response->ErrorCode});
        })) {}
    void Read(uint32_t cookie, uint32_t flags = TCommandRequest::EConsistent) {
        auto command = NewHoldedMessage<TCommandRequest>(sizeof(TCommandRequest) + sizeof(int64_t));
        int64_t index = 0;
        memcpy(command->Data, &index, sizeof(index));
        command->Cookie = cookie;
        command->Flags = flags;
        Processor.OnCommandRequest(command, Endpoint);
    }
    void Poll() { Processor.ProcessCommitted(); Processor.ProcessWaiting(); }
};

struct TEchoRsm : IRsm {
    std::vector<std::string> Applied;
    std::weak_ptr<char[]> LastReply;
    TMessageHolder<TMessage> Read(TMessageHolder<TCommandRequest>, uint64_t index) override {
        return NewHoldedMessage(TCommandResponse{.Index = index});
    }
    TMessageHolder<TLogEntry> Prepare(TMessageHolder<TCommandRequest> command) override {
        const auto size = command->Len - sizeof(TCommandRequest);
        auto entry = NewHoldedMessage<TLogEntry>(sizeof(TLogEntry) + size);
        memcpy(entry->Data, command->Data, size);
        return entry;
    }
    TMessageHolder<TMessage> Write(TMessageHolder<TLogEntry> entry, uint64_t index) override {
        const auto size = entry->Len - sizeof(TLogEntry);
        Applied.emplace_back(entry->Data, size);
        LastAppliedIndex = index;
        auto reply = NewHoldedMessage<TCommandResponse>(sizeof(TCommandResponse) + size);
        reply->Index = index;
        memcpy(reply->Data, entry->Data, size);
        LastReply = reply.RawData;
        return reply;
    }
};
struct TWriteClient {
    struct TReply { uint32_t Cookie, Error; std::string Data; };
    std::vector<TReply> Replies;
    std::shared_ptr<TEchoRsm> Rsm = std::make_shared<TEchoRsm>();
    TRequestProcessor Processor;
    std::shared_ptr<TEndpoint> Endpoint;
    explicit TWriteClient(std::shared_ptr<TRaft> raft)
        : Processor(raft, Rsm, {})
        , Endpoint(std::make_shared<TEndpoint>([this](auto message) {
            auto response = message.template Cast<TCommandResponse>();
            Replies.push_back({response->Cookie, response->ErrorCode,
                std::string(response->Data, response->Len - sizeof(TCommandResponse))});
        })) {}
    void Write(uint32_t cookie, const std::string& value) {
        auto command = NewHoldedMessage<TCommandRequest>(sizeof(TCommandRequest) + value.size());
        command->Cookie = cookie;
        command->Flags = TCommandRequest::EWrite;
        memcpy(command->Data, value.data(), value.size());
        Processor.OnCommandRequest(command, Endpoint);
    }
    void Read(uint32_t cookie, uint32_t flags = 0) {
        auto command = NewHoldedMessage<TCommandRequest>();
        command->Cookie = cookie;
        command->Flags = flags;
        Processor.OnCommandRequest(command, Endpoint);
    }
    void Poll() { Processor.ProcessCommitted(); Processor.ProcessWaiting(); }
};
TMessageHolder<TLogEntry> LogValue(const std::string& value) {
    auto entry = NewHoldedMessage<TLogEntry>(sizeof(TLogEntry) + value.size());
    memcpy(entry->Data, value.data(), value.size());
    return entry;
}

void test_nonleader_proposal_rejection_preserves_store_and_input(void**) {
    for (int phase = 0; phase < 3; ++phase) {
        TCluster cluster(3);
        auto raft = cluster.Nodes[0];
        if (phase == 1) {
            raft->ProcessTimeout(cluster.Now);
            assert_int_equal(raft->CurrentStateName(), EState::CANDIDATE);
        } else if (phase == 2) {
            cluster.Elect(1); cluster.Replicate(1);
            auto vote = NewHoldedMessage(
                TMessageEx{.Src = 2, .Dst = 1, .Term = raft->GetState()->CurrentTerm + 1},
                TRequestVoteRequest{.LastLogIndex = raft->GetLastIndex(),
                    .LastLogTerm = raft->GetState()->LastLogTerm, .CandidateId = 2});
            raft->Process(cluster.Now, vote);
            assert_int_equal(raft->CurrentStateName(), EState::FOLLOWER);
        }
        const auto before = static_cast<TStateFields>(*raft->GetState());
        auto entry = LogValue("must not be proposed"); entry->Term = 77;
        bool rejected = false;
        try { raft->Append(entry); } catch (const std::logic_error&) { rejected = true; }
        assert_true(rejected);
        assert_true(static_cast<TStateFields>(*raft->GetState()) == before);
        assert_int_equal(entry->Term, 77);
    }
}

void test_follower_proposal_cannot_diverge_same_term_committed_command(void**) {
    TCluster cluster(3); cluster.Elect(1); cluster.Replicate(1);
    TWriteClient leader(cluster.Nodes[0]), follower(cluster.Nodes[1]), other(cluster.Nodes[2]);
    bool rejected = false;
    try { cluster.Nodes[1]->Append(LogValue("rogue")); }
    catch (const std::logic_error&) { rejected = true; }
    leader.Write(77, "valid"); // Same index/term as the unauthorized follower entry on old code.
    cluster.Replicate(1, true, true);
    leader.Poll(); follower.Poll(); other.Poll();
    for (auto* client : {&leader, &follower, &other}) {
        assert_int_equal(client->Rsm->Applied.size(), 1);
        assert_string_equal(client->Rsm->Applied[0].c_str(), "valid");
    }
    cluster.AssertCommittedPrefix();
    assert_true(rejected);
    assert_int_equal(leader.Replies.size(), 1);
    assert_int_equal(leader.Replies[0].Error, 0);
}

void test_forwarded_write_and_strong_read_complete_through_real_raft(void**) {
    TCluster cluster(3);
    cluster.Elect(1);
    cluster.Replicate(1);
    std::deque<TMessageHolder<TCommandRequest>> requests;
    std::deque<TMessageHolder<TCommandResponse>> responses;
    auto peer = std::make_shared<TEndpoint>([&](auto message) {
        requests.push_back(message.template Cast<TCommandRequest>());
    });
    auto returnPath = std::make_shared<TEndpoint>([&](auto message) {
        responses.push_back(message.template Cast<TCommandResponse>());
    });
    auto leaderRsm = std::make_shared<TEchoRsm>();
    auto originRsm = std::make_shared<TEchoRsm>();
    TRequestProcessor leader(cluster.Nodes[0], leaderRsm, {});
    TRequestProcessor origin(cluster.Nodes[1], originRsm, TNodeDict{{1, peer}});
    std::vector<uint32_t> cookies;
    auto client = std::make_shared<TEndpoint>([&](auto message) {
        auto response = message.template Cast<TCommandResponse>();
        assert_int_equal(response->ErrorCode, 0);
        cookies.push_back(response->Cookie);
    });
    for (uint32_t cookie : {42, 77}) {
        auto command = NewHoldedMessage<TCommandRequest>(sizeof(TCommandRequest) + 5);
        command->Cookie = cookie;
        command->Flags = cookie == 42 ? TCommandRequest::EWrite : TCommandRequest::EConsistent;
        memcpy(command->Data, "value", 5);
        origin.OnCommandRequest(command, client);
        assert_int_equal(requests.size(), 1);
        leader.OnCommandRequest(requests.front(), returnPath);
        requests.pop_front();
        cluster.Replicate(1, true, true);
        leader.ProcessCommitted();
        leader.ProcessWaiting();
        assert_int_equal(responses.size(), 1);
        origin.OnCommandResponse(responses.front(), peer);
        origin.OnCommandResponse(responses.front(), peer);
        responses.pop_front();
        assert_int_equal(cookies.back(), cookie);
    }
    assert_int_equal(cookies.size(), 2);
    assert_int_equal(leaderRsm->Applied.size(), 1);
    assert_string_equal(leaderRsm->Applied[0].c_str(), "value");
    assert_true(originRsm->Applied.empty());
}

void test_replaced_write_never_receives_another_commands_reply(void**) {
    TCluster cluster(3);
    cluster.Elect(1);
    cluster.Replicate(1);
    cluster.Nodes[0]->Append(LogValue("uncommitted-padding"));
    TWriteClient client(cluster.Nodes[0]);
    client.Write(11, "lost-command"); // index 3, old term
    cluster.Isolated = {1};
    cluster.Elect(2); // new-term no-op at index 2
    cluster.Nodes[1]->Append(LogValue("replacement-command")); // index 3, new term
    cluster.Replicate(2);
    cluster.Isolated.clear();
    cluster.Replicate(2);
    client.Poll();
    assert_int_equal(client.Replies.size(), 1);
    assert_int_equal(client.Replies[0].Cookie, 11);
    assert_int_equal(client.Replies[0].Error, 1);
    assert_true(client.Replies[0].Data.empty());
    assert_int_equal(client.Rsm->Applied.size(), 1);
    assert_string_equal(client.Rsm->Applied[0].c_str(), "replacement-command");
    client.Poll();
    assert_int_equal(client.Replies.size(), 1);
}

void test_stepdown_rejects_write_without_waiting_for_application(void**) {
    TCluster cluster(3);
    cluster.Elect(1);
    cluster.Replicate(1);
    TWriteClient client(cluster.Nodes[0]);
    client.Write(11, "pending");
    cluster.Elect(2);
    client.Processor.CheckStateChange();
    assert_int_equal(client.Replies.size(), 1);
    assert_int_equal(client.Replies[0].Error, 1);
}

void test_committed_but_unanswered_write_has_unknown_outcome_on_stepdown(void**) {
    TCluster cluster(3);
    cluster.Elect(1);
    cluster.Replicate(1);
    TWriteClient client(cluster.Nodes[0]);
    client.Write(11, "committed");
    cluster.Replicate(1);
    client.Processor.ProcessCommitted(); // application reply cached, not sent
    assert_false(client.Rsm->LastReply.expired());
    cluster.Elect(2);
    client.Poll();
    assert_int_equal(client.Replies.size(), 1);
    assert_int_equal(client.Replies[0].Error, 1);
    assert_int_equal(client.Rsm->Applied.size(), 1);
    assert_string_equal(client.Rsm->Applied[0].c_str(), "committed");
    assert_true(client.Rsm->LastReply.expired());
    client.Poll();
    assert_int_equal(client.Rsm->Applied.size(), 1);
    assert_int_equal(client.Replies.size(), 1);
}

void test_old_write_is_rejected_after_unobserved_reelection(void**) {
    TCluster cluster(3);
    cluster.Elect(1);
    cluster.Replicate(1);
    TWriteClient client(cluster.Nodes[0]);
    client.Write(11, "lost");
    cluster.Elect(2);
    cluster.Elect(1);
    cluster.Replicate(1);
    client.Write(22, "new");
    assert_int_equal(client.Replies.size(), 1);
    assert_int_equal(client.Replies[0].Cookie, 11);
    assert_int_equal(client.Replies[0].Error, 1);
    cluster.Replicate(1);
    client.Poll();
    assert_int_equal(client.Replies.size(), 2);
    assert_int_equal(client.Replies[1].Cookie, 22);
    assert_int_equal(client.Replies[1].Error, 0);
    assert_string_equal(client.Replies[1].Data.c_str(), "new");
}

void test_default_read_of_lost_tail_is_rejected_on_stepdown(void**) {
    TCluster cluster(3);
    cluster.Elect(1);
    cluster.Replicate(1);
    cluster.Nodes[0]->Append(LogValue("lost-1"));
    cluster.Nodes[0]->Append(LogValue("lost-2"));
    TWriteClient client(cluster.Nodes[0]);
    client.Read(11);
    cluster.Elect(2);
    client.Processor.CheckStateChange();
    assert_int_equal(client.Replies.size(), 1);
    assert_int_equal(client.Replies[0].Error, 1);
}

void test_request_waiting_for_leader_runs_when_local_node_is_elected(void**) {
    for (uint32_t count : {1, 3}) {
        TCluster cluster(count);
        TWriteClient client(cluster.Nodes[0]);
        client.Write(0, "waiting-for-election");
        cluster.Elect(1);
        client.Processor.CheckStateChange();
        assert_int_equal(cluster.Nodes[0]->GetLastIndex(), 2);
        cluster.Replicate(1);
        client.Poll();
        assert_int_equal(client.Replies.size(), 1);
        assert_int_equal(client.Replies[0].Error, 0);
        assert_string_equal(client.Replies[0].Data.c_str(), "waiting-for-election");
    }
}

void test_write_replaced_by_noop_is_rejected_without_application_reply(void**) {
    TCluster cluster(3);
    cluster.Elect(1);
    cluster.Replicate(1);
    TWriteClient client(cluster.Nodes[0]);
    client.Write(11, "lost");
    cluster.Elect(2);
    cluster.Replicate(2);
    client.Poll();
    assert_int_equal(client.Replies.size(), 1);
    assert_int_equal(client.Replies[0].Cookie, 11);
    assert_int_equal(client.Replies[0].Error, 1);
    assert_true(client.Rsm->Applied.empty());
}

void test_stale_read_does_not_wait_for_uncommitted_tail(void**) {
    TCluster cluster(3);
    cluster.Elect(1);
    cluster.Replicate(1);
    cluster.Nodes[0]->Append(LogValue("uncommitted"));
    TWriteClient client(cluster.Nodes[0]);
    client.Read(11, TCommandRequest::EStale);
    client.Poll();
    assert_int_equal(client.Replies.size(), 1);
    assert_int_equal(client.Replies[0].Error, 0);
    assert_true(client.Rsm->Applied.empty());
}

void test_disconnect_removes_local_write_and_read_waiters(void**) {
    TCluster cluster(3);
    cluster.Elect(1);
    cluster.Replicate(1);
    TWriteClient client(cluster.Nodes[0]);
    client.Write(11, "still-may-commit");
    client.Read(22);
    client.Read(33, TCommandRequest::EConsistent);
    std::vector<uint32_t> otherReplies;
    auto other = std::make_shared<TEndpoint>([&](auto message) {
        auto reply = message.template Cast<TCommandResponse>();
        assert_int_equal(reply->ErrorCode, 0);
        otherReplies.push_back(reply->Cookie);
    });
    auto command = NewHoldedMessage<TCommandRequest>(sizeof(TCommandRequest) + 5);
    command->Cookie = 44;
    command->Flags = TCommandRequest::EWrite;
    memcpy(command->Data, "other", 5);
    client.Processor.OnCommandRequest(command, other);
    std::weak_ptr<INode> endpoint = client.Endpoint;
    client.Processor.CleanUp(client.Endpoint);
    client.Endpoint.reset();
    assert_true(endpoint.expired());
    cluster.Replicate(1);
    client.Poll();
    assert_true(client.Replies.empty());
    assert_int_equal(client.Rsm->Applied.size(), 2);
    assert_int_equal(otherReplies.size(), 1);
    assert_int_equal(otherReplies[0], 44);
    assert_true(client.Rsm->LastReply.expired());
}

void test_disconnect_removes_requests_waiting_for_leader(void**) {
    TCluster cluster(3);
    TWriteClient client(cluster.Nodes[0]);
    client.Write(0, "disconnected-before-election");
    std::weak_ptr<INode> endpoint = client.Endpoint;
    client.Processor.CleanUp(client.Endpoint);
    client.Endpoint.reset();
    assert_true(endpoint.expired());
    cluster.Elect(1);
    client.Processor.CheckStateChange();
    assert_int_equal(cluster.Nodes[0]->GetLastIndex(), 1);
}

void test_disconnect_releases_cached_write_reply(void**) {
    TCluster cluster(3);
    cluster.Elect(1);
    cluster.Replicate(1);
    TWriteClient client(cluster.Nodes[0]);
    client.Write(11, "committed");
    cluster.Replicate(1);
    client.Processor.ProcessCommitted();
    assert_false(client.Rsm->LastReply.expired());
    client.Processor.CleanUp(client.Endpoint);
    assert_true(client.Rsm->LastReply.expired());
    client.Poll();
    assert_true(client.Replies.empty());
}

void test_follower_application_does_not_retain_unused_replies(void**) {
    TCluster cluster(3);
    cluster.Elect(1);
    cluster.Replicate(1);
    TWriteClient follower(cluster.Nodes[1]);
    cluster.Nodes[0]->Append(LogValue("background-command"));
    cluster.Replicate(1);
    follower.Processor.ProcessCommitted();
    assert_int_equal(follower.Rsm->Applied.size(), 1);
    assert_true(follower.Rsm->LastReply.expired());
}

void test_strong_reads_require_fresh_majority_in_all_cluster_sizes(void**) {
    for (uint32_t count = 1; count <= 8; ++count) {
        TCluster cluster(count);
        cluster.Elect(1);
        cluster.Replicate(1);
        TReadClient client(cluster.Nodes[0]);
        client.Read(11);
        client.Poll();
        assert_int_equal(client.Replies.size(), count == 1 ? 1 : 0);
        for (uint32_t peer = 2; peer <= count; ++peer) {
            cluster.RoundTrip(1, peer, true);
            client.Poll();
            assert_int_equal(client.Replies.size(), peer >= count / 2 + 1 ? 1 : 0);
        }
        assert_int_equal(client.Replies[0].Cookie, 11);
        assert_int_equal(client.Replies[0].Error, 0);
    }
}

void test_stale_leader_with_one_follower_cannot_serve_strong_read(void**) {
    TCluster cluster(5);
    cluster.Elect(1);
    cluster.Replicate(1);
    // The majority elects a new leader and commits a later entry while the
    // old leader remains connected to one old-term follower.
    cluster.Isolated = {1, 2};
    cluster.Elect(3);
    auto latest = cluster.Nodes[2]->Append(NewHoldedMessage<TLogEntry>());
    cluster.Replicate(3);
    assert_true(cluster.Nodes[2]->GetVolatileState()->CommitIndex >= latest);
    cluster.Isolated.clear();
    TReadClient client(cluster.Nodes[0]);
    client.Read(11);
    cluster.RoundTrip(1, 2, true);
    client.Poll();
    assert_true(client.Replies.empty());
    // A current-term leader's follower reveals the higher term. Pending reads
    // must fail, rather than serving the old leader's state.
    cluster.RoundTrip(1, 3);
    client.Poll();
    assert_int_equal(client.Replies.size(), 1);
    assert_int_equal(client.Replies[0].Error, 1);
}

void test_new_read_cannot_reuse_previous_read_quorum(void**) {
    TCluster cluster(5);
    cluster.Elect(1);
    cluster.Replicate(1);
    TReadClient client(cluster.Nodes[0]);
    client.Read(11);
    cluster.RoundTrip(1, 2);
    cluster.RoundTrip(1, 3);
    client.Poll();
    assert_int_equal(client.Replies.size(), 1);
    client.Read(22);
    cluster.RoundTrip(1, 2, true);
    client.Poll();
    assert_int_equal(client.Replies.size(), 1);
    cluster.RoundTrip(1, 3);
    client.Poll();
    assert_int_equal(client.Replies.size(), 2);
    assert_int_equal(client.Replies[1].Cookie, 22);
}

void test_ready_read_is_cancelled_on_leadership_change(void**) {
    TCluster cluster(3);
    cluster.Elect(1);
    cluster.Replicate(1);
    TReadClient client(cluster.Nodes[0]);
    client.Read(11);
    cluster.RoundTrip(1, 2);
    cluster.Elect(2); // Lose leadership before the application processes reads.
    client.Poll();
    assert_int_equal(client.Replies.size(), 1);
    assert_int_equal(client.Replies[0].Error, 1);
}

void test_consistent_flag_takes_precedence_over_stale(void**) {
    TCluster cluster(3);
    cluster.Elect(1);
    cluster.Replicate(1);
    TReadClient client(cluster.Nodes[0]);
    client.Read(11, TCommandRequest::EConsistent | TCommandRequest::EStale);
    client.Poll();
    assert_true(client.Replies.empty());
    cluster.RoundTrip(1, 2);
    client.Poll();
    assert_int_equal(client.Replies.size(), 1);
    assert_int_equal(client.Replies[0].Error, 0);
}

void test_delayed_pre_read_replies_cannot_confirm_new_read(void**) {
    TCluster cluster(5);
    cluster.Elect(1);
    cluster.Replicate(1);
    cluster.Nodes[0]->ProcessTimeout(cluster.Now += TTimeout::Election);
    for (uint32_t peer = 2; peer <= 5; ++peer) cluster.DeliverOne(1, peer);
    TReadClient client(cluster.Nodes[0]);
    client.Read(11);
    for (uint32_t peer = 2; peer <= 5; ++peer) cluster.DeliverOne(peer, 1);
    client.Poll();
    assert_true(client.Replies.empty());
    cluster.RoundTrip(1, 2, true);
    client.Poll();
    assert_true(client.Replies.empty());
    cluster.RoundTrip(1, 3);
    client.Poll();
    assert_int_equal(client.Replies.size(), 1);
}

void test_new_leader_waits_for_current_term_commit_and_application(void**) {
    TCluster cluster(3);
    for (auto& node : cluster.Nodes) {
        node->GetState()->Append(NewHoldedMessage<TLogEntry>());
        const_cast<TVolatileState*>(node->GetVolatileState())->CommitIndex = 1;
    }
    cluster.Elect(1);
    // First RPCs probe the old prefix; the new-term no-op is not committed yet.
    assert_int_equal(cluster.Nodes[0]->GetVolatileState()->CommitIndex, 1);
    assert_int_equal(cluster.Nodes[0]->CommitSeqno(), 0);
    TReadClient client(cluster.Nodes[0]);
    client.Read(11);
    client.Poll();
    assert_true(client.Replies.empty());
    cluster.RoundTrip(1, 2);
    assert_int_equal(cluster.Nodes[0]->GetVolatileState()->CommitIndex, 2);
    // Fresh confirmation alone is insufficient until the local state machine
    // has applied the read's target prefix.
    client.Processor.ProcessWaiting();
    assert_true(client.Replies.empty());
    client.Poll();
    assert_int_equal(client.Replies.size(), 1);
    assert_int_equal(client.Replies[0].Error, 0);
}

void test_old_term_read_is_rejected_after_reelection(void**) {
    TCluster cluster(3);
    cluster.Elect(1);
    cluster.Replicate(1);
    TReadClient client(cluster.Nodes[0]);
    client.Read(11);
    cluster.Elect(2);
    cluster.Elect(1);
    cluster.Replicate(1);
    // No application poll observed the intermediate follower state. The term
    // tag must still reject the old operation before accepting another read.
    client.Read(22);
    assert_int_equal(client.Replies.size(), 1);
    assert_int_equal(client.Replies[0].Cookie, 11);
    assert_int_equal(client.Replies[0].Error, 1);
    cluster.RoundTrip(1, 2);
    client.Poll();
    assert_int_equal(client.Replies.size(), 2);
    assert_int_equal(client.Replies[1].Cookie, 22);
    assert_int_equal(client.Replies[1].Error, 0);
}

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
    if (flags & TCommandRequest::EConsistent) raft->ProcessTimeout(ITimeSource::Time{});
    else raft->Become(EState::LEADER);
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

void test_follower_restarted_without_storage_catches_up(void**) {
    TCluster cluster(3);
    cluster.Elect(1);
    for (int i = 0; i < 3; ++i) cluster.Nodes[0]->Append(NewHoldedMessage<TLogEntry>());
    cluster.Replicate(1);
    const auto last = cluster.Nodes[0]->GetState()->LastLogIndex;
    const auto committed = cluster.Nodes[0]->GetVolatileState()->CommitIndex;
    assert_int_equal(cluster.Nodes[2]->GetState()->LastLogIndex, last);
    // Node 3 restarts with an empty in-memory log while node 1 stays leader.
    TNodeDict peers;
    for (uint32_t dst : {1u, 2u}) {
        peers[dst] = std::make_shared<TEndpoint>([&cluster](auto m) { cluster.Packets.push_back(std::move(m)); });
    }
    cluster.Nodes[2] = std::make_shared<TRaft>(std::make_shared<TState>(), 3, peers);
    cluster.Replicate(1);
    assert_int_equal(cluster.Nodes[0]->CurrentStateName(), EState::LEADER);
    assert_int_equal(cluster.Nodes[2]->GetState()->LastLogIndex, last);
    assert_int_equal(cluster.Nodes[0]->GetVolatileState()->MatchIndex.at(3), last);
    assert_true(cluster.Nodes[0]->GetVolatileState()->CommitIndex >= committed);
    cluster.AssertCommittedPrefix();
}

std::shared_ptr<TEchoRsm> RecoverApplication(const TEchoRsm& old) {
    auto recovered = std::make_shared<TEchoRsm>();
    recovered->Applied = old.Applied;
    recovered->LastAppliedIndex = old.LastAppliedIndex;
    return recovered;
}
void test_recovered_application_progress_is_not_rewound(void**) {
    TCluster cluster(1);
    cluster.Elect(1);
    cluster.Nodes[0]->Append(LogValue("once"));
    cluster.Replicate(1);
    auto original = std::make_shared<TEchoRsm>();
    { TRequestProcessor processor(cluster.Nodes[0], original, {}); processor.ProcessCommitted(); }
    auto recovered = RecoverApplication(*original);
    cluster.Restart(1);
    TRequestProcessor processor(cluster.Nodes[0], recovered, {});
    processor.ProcessCommitted(); // timer before the first post-restart election
    assert_int_equal(recovered->LastAppliedIndex, original->LastAppliedIndex);
    cluster.Elect(1);
    processor.ProcessCommitted();
    processor.ProcessCommitted();
    assert_true(recovered->Applied == std::vector<std::string>{"once"});
}
void test_application_progress_never_decreases_on_lower_commit_input(void**) {
    TCluster cluster(1);
    cluster.Elect(1);
    auto recovered = std::make_shared<TEchoRsm>();
    recovered->LastAppliedIndex = 1;
    TRequestProcessor processor(cluster.Nodes[0], recovered, {});
    // Defensive check even if an embedder violates commit-index monotonicity.
    const_cast<TVolatileState*>(cluster.Nodes[0]->GetVolatileState())->CommitIndex = 0;
    processor.ProcessCommitted();
    assert_int_equal(recovered->LastAppliedIndex, 1);
}
void test_recovered_applied_prefix_is_protected_and_suffix_can_change(void**) {
    TCluster cluster(2);
    auto state = cluster.Nodes[0]->GetState();
    state->CurrentTerm = 2;
    for (const std::string value : {"applied", "uncommitted"}) {
        auto entry = LogValue(value);
        entry->Term = 2;
        state->Append(entry);
    }
    auto recovered = std::make_shared<TEchoRsm>();
    recovered->Applied = {"applied"};
    recovered->LastAppliedIndex = 1;
    TRequestProcessor processor(cluster.Nodes[0], recovered, {});
    auto request = [&](uint64_t prev, uint64_t prevTerm, const std::string& value) {
        auto message = NewHoldedMessage(TMessageEx{.Src = 2, .Dst = 1, .Term = 3},
            TAppendEntriesRequest{.PrevLogIndex = prev, .PrevLogTerm = prevTerm,
                .LeaderCommit = 2, .LeaderId = 2, .Nentries = 1});
        message.InitPayload(1);
        auto entry = LogValue(value);
        entry->Term = 3;
        message.Payload[0] = entry;
        return message;
    };
    cluster.Nodes[0]->Process(cluster.Now, request(0, 0, "overwrite-applied"));
    assert_int_equal(state->LogTerm(1), 2);
    assert_int_equal(cluster.Nodes[0]->GetVolatileState()->CommitIndex, 1);
    cluster.Nodes[0]->Process(cluster.Now, request(1, 2, "replacement"));
    processor.ProcessCommitted();
    assert_true(recovered->Applied == (std::vector<std::string>{"applied", "replacement"}));
    assert_int_equal(recovered->LastAppliedIndex, 2);
}
void test_application_ahead_of_recovered_log_is_rejected(void**) {
    TCluster cluster(1);
    for (const auto index : {uint64_t(1), UINT64_MAX}) {
        auto recovered = std::make_shared<TEchoRsm>();
        recovered->LastAppliedIndex = index;
        bool rejected = false;
        try { TRequestProcessor processor(cluster.Nodes[0], recovered, {}); }
        catch (const std::invalid_argument&) { rejected = true; }
        assert_true(rejected);
        assert_true(recovered->LastAppliedIndex == index);
        assert_int_equal(cluster.Nodes[0]->GetVolatileState()->CommitIndex, 0);
        assert_int_equal(cluster.Nodes[0]->GetLastIndex(), 0);
    }
}
void test_cluster_restart_preserves_applied_history_and_rebuilds_volatile_application(void**) {
    TCluster cluster(3);
    cluster.Elect(1);
    cluster.Nodes[0]->Append(LogValue("first"));
    cluster.Replicate(1);
    std::vector<std::shared_ptr<TEchoRsm>> recovered;
    for (auto& node : cluster.Nodes) {
        auto rsm = std::make_shared<TEchoRsm>();
        TRequestProcessor processor(node, rsm, {});
        processor.ProcessCommitted();
        recovered.push_back(RecoverApplication(*rsm));
    }
    cluster.Nodes[0]->Append(LogValue("old-uncommitted-tail"));
    // Keep delayed old-term traffic across all node restarts.
    cluster.Nodes[0]->ProcessTimeout(cluster.Now + std::chrono::seconds(1));
    auto delayed = std::move(cluster.Packets);
    cluster.Packets.clear();
    for (uint32_t id = 1; id <= 3; ++id) cluster.Restart(id);
    // Node 3 instead uses a volatile application and must rebuild from index zero.
    recovered[2] = std::make_shared<TEchoRsm>();
    std::vector<std::unique_ptr<TRequestProcessor>> processors;
    for (size_t i = 0; i < recovered.size(); ++i) {
        processors.push_back(std::make_unique<TRequestProcessor>(cluster.Nodes[i], recovered[i], TNodeDict{}));
        processors.back()->ProcessCommitted();
    }
    cluster.Elect(2);
    // Recovered old-term commitment cannot replace a fresh current-term read quorum.
    assert_int_equal(cluster.Nodes[1]->CommitSeqno(), 0);
    cluster.Nodes[1]->Append(LogValue("second"));
    cluster.Replicate(2, true, true);
    cluster.Packets = std::move(delayed);
    cluster.Drain(true, true);
    cluster.Replicate(2, true, true);
    for (auto& processor : processors) { processor->ProcessCommitted(); processor->ProcessCommitted(); }
    for (const auto& rsm : recovered) {
        assert_true(rsm->Applied == (std::vector<std::string>{"first", "second"}));
        assert_int_equal(rsm->LastAppliedIndex, cluster.Nodes[1]->GetVolatileState()->CommitIndex);
    }
    cluster.AssertCommittedPrefix();
}

}

int main() {
    const CMUnitTest tests[] = {
        cmocka_unit_test(test_nonleader_proposal_rejection_preserves_store_and_input),
        cmocka_unit_test(test_follower_proposal_cannot_diverge_same_term_committed_command),
        cmocka_unit_test(test_recovered_application_progress_is_not_rewound),
        cmocka_unit_test(test_application_progress_never_decreases_on_lower_commit_input),
        cmocka_unit_test(test_recovered_applied_prefix_is_protected_and_suffix_can_change),
        cmocka_unit_test(test_application_ahead_of_recovered_log_is_rejected),
        cmocka_unit_test(test_cluster_restart_preserves_applied_history_and_rebuilds_volatile_application),
        cmocka_unit_test(test_majority_all_cluster_sizes),
        cmocka_unit_test(test_read_fifo),
        cmocka_unit_test(test_strong_read_fifo),
        cmocka_unit_test(test_write_fifo),
        cmocka_unit_test(test_partial_write_completion),
        cmocka_unit_test(test_loss_reorder_duplicate_and_recovery),
        cmocka_unit_test(test_partition_leader_replacement_and_heal),
        cmocka_unit_test(test_even_cluster_partition_requires_majority),
        cmocka_unit_test(test_follower_restarted_without_storage_catches_up),
        cmocka_unit_test(test_strong_reads_require_fresh_majority_in_all_cluster_sizes),
        cmocka_unit_test(test_stale_leader_with_one_follower_cannot_serve_strong_read),
        cmocka_unit_test(test_new_read_cannot_reuse_previous_read_quorum),
        cmocka_unit_test(test_ready_read_is_cancelled_on_leadership_change),
        cmocka_unit_test(test_consistent_flag_takes_precedence_over_stale),
        cmocka_unit_test(test_delayed_pre_read_replies_cannot_confirm_new_read),
        cmocka_unit_test(test_new_leader_waits_for_current_term_commit_and_application),
        cmocka_unit_test(test_old_term_read_is_rejected_after_reelection),
        cmocka_unit_test(test_replaced_write_never_receives_another_commands_reply),
        cmocka_unit_test(test_stepdown_rejects_write_without_waiting_for_application),
        cmocka_unit_test(test_committed_but_unanswered_write_has_unknown_outcome_on_stepdown),
        cmocka_unit_test(test_old_write_is_rejected_after_unobserved_reelection),
        cmocka_unit_test(test_default_read_of_lost_tail_is_rejected_on_stepdown),
        cmocka_unit_test(test_request_waiting_for_leader_runs_when_local_node_is_elected),
        cmocka_unit_test(test_disconnect_removes_local_write_and_read_waiters),
        cmocka_unit_test(test_disconnect_removes_requests_waiting_for_leader),
        cmocka_unit_test(test_disconnect_releases_cached_write_reply),
        cmocka_unit_test(test_follower_application_does_not_retain_unused_replies),
        cmocka_unit_test(test_write_replaced_by_noop_is_rejected_without_application_reply),
        cmocka_unit_test(test_stale_read_does_not_wait_for_uncommitted_tail),
        cmocka_unit_test(test_forwarded_write_and_strong_read_complete_through_real_raft),
    };
    return cmocka_run_group_tests(tests, nullptr, nullptr);
}
