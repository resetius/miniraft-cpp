#include <miniraft/raft.h>
#include <deque>
#include <functional>
#include <set>
#include <gtest/gtest.h>

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
            ASSERT_TRUE(budget-- > 0);
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
        ASSERT_NO_FATAL_FAILURE(Drain());
        node->ProcessTimeout(Now);
        ASSERT_NO_FATAL_FAILURE(Drain());
        ASSERT_EQ(node->CurrentStateName(), EState::LEADER);
    }
    void Replicate(uint32_t id, bool reverse = false, bool duplicate = false) {
        for (int round = 0; round < 12; ++round) {
            Now += std::chrono::milliseconds(500);
            Nodes.at(id - 1)->ProcessTimeout(Now);
            ASSERT_NO_FATAL_FAILURE(Drain(reverse, duplicate));
        }
    }
    void AssertCommittedPrefix() {
        for (auto& a : Nodes) for (auto& b : Nodes) {
            auto end = std::min(a->GetVolatileState()->CommitIndex, b->GetVolatileState()->CommitIndex);
            for (uint64_t i = 1; i <= end; ++i) {
                auto x = a->GetState()->Get(i), y = b->GetState()->Get(i);
                ASSERT_EQ(x->Len, y->Len);
                ASSERT_EQ(0, memcmp(x.Mes, y.Mes, x->Len));
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
        ADD_FAILURE() << "Expected a queued packet from " << src << " to " << dst;
        throw std::logic_error("no queued packet");
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
    TMessageHolder<TInstallSnapshotRequest> Snapshot() const override {
        throw std::runtime_error("Not implemented");
    }
    void Apply(TMessageHolder<TInstallSnapshotRequest>) override {
        throw std::runtime_error("Not implemented");
    }
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

TEST(Correctness, nonleader_proposal_rejection_preserves_store_and_input) {
    for (int phase = 0; phase < 3; ++phase) {
        TCluster cluster(3);
        auto raft = cluster.Nodes[0];
        if (phase == 1) {
            raft->ProcessTimeout(cluster.Now);
            ASSERT_EQ(raft->CurrentStateName(), EState::CANDIDATE);
        } else if (phase == 2) {
            ASSERT_NO_FATAL_FAILURE(cluster.Elect(1); cluster.Replicate(1));
            // A leader ignores vote requests; a reply with a newer term makes it step down.
            auto newer = NewHoldedMessage(
                TMessageEx{.Src = 2, .Dst = 1, .Term = raft->GetState()->CurrentTerm + 1},
                TAppendEntriesResponse{.MatchIndex = 0, .Success = 0});
            raft->Process(cluster.Now, newer);
            ASSERT_EQ(raft->CurrentStateName(), EState::FOLLOWER);
        }
        const auto before = static_cast<TStateFields>(*raft->GetState());
        auto entry = LogValue("must not be proposed"); entry->Term = 77;
        bool rejected = false;
        try { raft->Append(entry); } catch (const std::logic_error&) { rejected = true; }
        ASSERT_TRUE(rejected);
        ASSERT_TRUE(static_cast<TStateFields>(*raft->GetState()) == before);
        ASSERT_EQ(entry->Term, 77);
    }
}

TEST(Correctness, follower_proposal_cannot_diverge_same_term_committed_command) {
    TCluster cluster(3);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    cluster.Replicate(1);
    TWriteClient leader(cluster.Nodes[0]), follower(cluster.Nodes[1]), other(cluster.Nodes[2]);
    bool rejected = false;
    try { cluster.Nodes[1]->Append(LogValue("rogue")); }
    catch (const std::logic_error&) { rejected = true; }
    leader.Write(77, "valid"); // Same index/term as the unauthorized follower entry on old code.
    cluster.Replicate(1, true, true);
    leader.Poll(); follower.Poll(); other.Poll();
    for (auto* client : {&leader, &follower, &other}) {
        ASSERT_EQ(client->Rsm->Applied.size(), 1);
        ASSERT_STREQ(client->Rsm->Applied[0].c_str(), "valid");
    }
    ASSERT_NO_FATAL_FAILURE(cluster.AssertCommittedPrefix());
    ASSERT_TRUE(rejected);
    ASSERT_EQ(leader.Replies.size(), 1);
    ASSERT_EQ(leader.Replies[0].Error, 0);
}

TEST(Correctness, forwarded_write_and_strong_read_complete_through_real_raft) {
    TCluster cluster(3);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
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
        ASSERT_EQ(response->ErrorCode, 0);
        cookies.push_back(response->Cookie);
    });
    for (uint32_t cookie : {42, 77}) {
        auto command = NewHoldedMessage<TCommandRequest>(sizeof(TCommandRequest) + 5);
        command->Cookie = cookie;
        command->Flags = cookie == 42 ? TCommandRequest::EWrite : TCommandRequest::EConsistent;
        memcpy(command->Data, "value", 5);
        origin.OnCommandRequest(command, client);
        ASSERT_EQ(requests.size(), 1);
        leader.OnCommandRequest(requests.front(), returnPath);
        requests.pop_front();
        cluster.Replicate(1, true, true);
        leader.ProcessCommitted();
        leader.ProcessWaiting();
        ASSERT_EQ(responses.size(), 1);
        origin.OnCommandResponse(responses.front(), peer);
        origin.OnCommandResponse(responses.front(), peer);
        responses.pop_front();
        ASSERT_EQ(cookies.back(), cookie);
    }
    ASSERT_EQ(cookies.size(), 2);
    ASSERT_EQ(leaderRsm->Applied.size(), 1);
    ASSERT_STREQ(leaderRsm->Applied[0].c_str(), "value");
    ASSERT_TRUE(originRsm->Applied.empty());
}

TEST(Correctness, replaced_write_never_receives_another_commands_reply) {
    TCluster cluster(3);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    cluster.Replicate(1);
    cluster.Nodes[0]->Append(LogValue("uncommitted-padding"));
    TWriteClient client(cluster.Nodes[0]);
    client.Write(11, "lost-command"); // index 3, old term
    cluster.Isolated = {1};
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(2)); // new-term no-op at index 2
    cluster.Nodes[1]->Append(LogValue("replacement-command")); // index 3, new term
    cluster.Replicate(2);
    cluster.Isolated.clear();
    cluster.Replicate(2);
    client.Poll();
    ASSERT_EQ(client.Replies.size(), 1);
    ASSERT_EQ(client.Replies[0].Cookie, 11);
    ASSERT_EQ(client.Replies[0].Error, 1);
    ASSERT_TRUE(client.Replies[0].Data.empty());
    ASSERT_EQ(client.Rsm->Applied.size(), 1);
    ASSERT_STREQ(client.Rsm->Applied[0].c_str(), "replacement-command");
    client.Poll();
    ASSERT_EQ(client.Replies.size(), 1);
}

TEST(Correctness, stepdown_rejects_write_without_waiting_for_application) {
    TCluster cluster(3);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    cluster.Replicate(1);
    TWriteClient client(cluster.Nodes[0]);
    client.Write(11, "pending");
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(2));
    client.Processor.CheckStateChange();
    ASSERT_EQ(client.Replies.size(), 1);
    ASSERT_EQ(client.Replies[0].Error, 1);
}

TEST(Correctness, committed_but_unanswered_write_has_unknown_outcome_on_stepdown) {
    TCluster cluster(3);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    cluster.Replicate(1);
    TWriteClient client(cluster.Nodes[0]);
    client.Write(11, "committed");
    cluster.Replicate(1);
    client.Processor.ProcessCommitted(); // application reply cached, not sent
    ASSERT_FALSE(client.Rsm->LastReply.expired());
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(2));
    client.Poll();
    ASSERT_EQ(client.Replies.size(), 1);
    ASSERT_EQ(client.Replies[0].Error, 1);
    ASSERT_EQ(client.Rsm->Applied.size(), 1);
    ASSERT_STREQ(client.Rsm->Applied[0].c_str(), "committed");
    ASSERT_TRUE(client.Rsm->LastReply.expired());
    client.Poll();
    ASSERT_EQ(client.Rsm->Applied.size(), 1);
    ASSERT_EQ(client.Replies.size(), 1);
}

TEST(Correctness, old_write_is_rejected_after_unobserved_reelection) {
    TCluster cluster(3);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    cluster.Replicate(1);
    TWriteClient client(cluster.Nodes[0]);
    client.Write(11, "lost");
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(2));
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    cluster.Replicate(1);
    client.Write(22, "new");
    ASSERT_EQ(client.Replies.size(), 1);
    ASSERT_EQ(client.Replies[0].Cookie, 11);
    ASSERT_EQ(client.Replies[0].Error, 1);
    cluster.Replicate(1);
    client.Poll();
    ASSERT_EQ(client.Replies.size(), 2);
    ASSERT_EQ(client.Replies[1].Cookie, 22);
    ASSERT_EQ(client.Replies[1].Error, 0);
    ASSERT_STREQ(client.Replies[1].Data.c_str(), "new");
}

TEST(Correctness, default_read_of_lost_tail_is_rejected_on_stepdown) {
    TCluster cluster(3);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    cluster.Replicate(1);
    cluster.Nodes[0]->Append(LogValue("lost-1"));
    cluster.Nodes[0]->Append(LogValue("lost-2"));
    TWriteClient client(cluster.Nodes[0]);
    client.Read(11);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(2));
    client.Processor.CheckStateChange();
    ASSERT_EQ(client.Replies.size(), 1);
    ASSERT_EQ(client.Replies[0].Error, 1);
}

TEST(Correctness, request_waiting_for_leader_runs_when_local_node_is_elected) {
    for (uint32_t count : {1, 3}) {
        TCluster cluster(count);
        TWriteClient client(cluster.Nodes[0]);
        client.Write(0, "waiting-for-election");
        ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
        client.Processor.CheckStateChange();
        ASSERT_EQ(cluster.Nodes[0]->GetLastIndex(), 2);
        cluster.Replicate(1);
        client.Poll();
        ASSERT_EQ(client.Replies.size(), 1);
        ASSERT_EQ(client.Replies[0].Error, 0);
        ASSERT_STREQ(client.Replies[0].Data.c_str(), "waiting-for-election");
    }
}

TEST(Correctness, write_replaced_by_noop_is_rejected_without_application_reply) {
    TCluster cluster(3);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    cluster.Replicate(1);
    TWriteClient client(cluster.Nodes[0]);
    client.Write(11, "lost");
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(2));
    cluster.Replicate(2);
    client.Poll();
    ASSERT_EQ(client.Replies.size(), 1);
    ASSERT_EQ(client.Replies[0].Cookie, 11);
    ASSERT_EQ(client.Replies[0].Error, 1);
    ASSERT_TRUE(client.Rsm->Applied.empty());
}

TEST(Correctness, stale_read_does_not_wait_for_uncommitted_tail) {
    TCluster cluster(3);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    cluster.Replicate(1);
    cluster.Nodes[0]->Append(LogValue("uncommitted"));
    TWriteClient client(cluster.Nodes[0]);
    client.Read(11, TCommandRequest::EStale);
    client.Poll();
    ASSERT_EQ(client.Replies.size(), 1);
    ASSERT_EQ(client.Replies[0].Error, 0);
    ASSERT_TRUE(client.Rsm->Applied.empty());
}

TEST(Correctness, disconnect_removes_local_write_and_read_waiters) {
    TCluster cluster(3);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    cluster.Replicate(1);
    TWriteClient client(cluster.Nodes[0]);
    client.Write(11, "still-may-commit");
    client.Read(22);
    client.Read(33, TCommandRequest::EConsistent);
    std::vector<uint32_t> otherReplies;
    auto other = std::make_shared<TEndpoint>([&](auto message) {
        auto reply = message.template Cast<TCommandResponse>();
        ASSERT_EQ(reply->ErrorCode, 0);
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
    ASSERT_TRUE(endpoint.expired());
    cluster.Replicate(1);
    client.Poll();
    ASSERT_TRUE(client.Replies.empty());
    ASSERT_EQ(client.Rsm->Applied.size(), 2);
    ASSERT_EQ(otherReplies.size(), 1);
    ASSERT_EQ(otherReplies[0], 44);
    ASSERT_TRUE(client.Rsm->LastReply.expired());
}

TEST(Correctness, disconnect_removes_requests_waiting_for_leader) {
    TCluster cluster(3);
    TWriteClient client(cluster.Nodes[0]);
    client.Write(0, "disconnected-before-election");
    std::weak_ptr<INode> endpoint = client.Endpoint;
    client.Processor.CleanUp(client.Endpoint);
    client.Endpoint.reset();
    ASSERT_TRUE(endpoint.expired());
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    client.Processor.CheckStateChange();
    ASSERT_EQ(cluster.Nodes[0]->GetLastIndex(), 1);
}

TEST(Correctness, disconnect_releases_cached_write_reply) {
    TCluster cluster(3);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    cluster.Replicate(1);
    TWriteClient client(cluster.Nodes[0]);
    client.Write(11, "committed");
    cluster.Replicate(1);
    client.Processor.ProcessCommitted();
    ASSERT_FALSE(client.Rsm->LastReply.expired());
    client.Processor.CleanUp(client.Endpoint);
    ASSERT_TRUE(client.Rsm->LastReply.expired());
    client.Poll();
    ASSERT_TRUE(client.Replies.empty());
}

TEST(Correctness, follower_application_does_not_retain_unused_replies) {
    TCluster cluster(3);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    cluster.Replicate(1);
    TWriteClient follower(cluster.Nodes[1]);
    cluster.Nodes[0]->Append(LogValue("background-command"));
    cluster.Replicate(1);
    follower.Processor.ProcessCommitted();
    ASSERT_EQ(follower.Rsm->Applied.size(), 1);
    ASSERT_TRUE(follower.Rsm->LastReply.expired());
}

TEST(Correctness, strong_reads_require_fresh_majority_in_all_cluster_sizes) {
    for (uint32_t count = 1; count <= 8; ++count) {
        TCluster cluster(count);
        ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
        cluster.Replicate(1);
        TReadClient client(cluster.Nodes[0]);
        client.Read(11);
        client.Poll();
        ASSERT_EQ(client.Replies.size(), count == 1 ? 1 : 0);
        for (uint32_t peer = 2; peer <= count; ++peer) {
            cluster.RoundTrip(1, peer, true);
            client.Poll();
            ASSERT_EQ(client.Replies.size(), peer >= count / 2 + 1 ? 1 : 0);
        }
        ASSERT_EQ(client.Replies[0].Cookie, 11);
        ASSERT_EQ(client.Replies[0].Error, 0);
    }
}

TEST(Correctness, stale_leader_with_one_follower_cannot_serve_strong_read) {
    TCluster cluster(5);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    cluster.Replicate(1);
    // The majority elects a new leader and commits a later entry while the
    // old leader remains connected to one old-term follower.
    cluster.Isolated = {1, 2};
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(3));
    auto latest = cluster.Nodes[2]->Append(NewHoldedMessage<TLogEntry>());
    cluster.Replicate(3);
    ASSERT_TRUE(cluster.Nodes[2]->GetVolatileState()->CommitIndex >= latest);
    cluster.Isolated.clear();
    TReadClient client(cluster.Nodes[0]);
    client.Read(11);
    cluster.RoundTrip(1, 2, true);
    client.Poll();
    ASSERT_TRUE(client.Replies.empty());
    // A current-term leader's follower reveals the higher term. Pending reads
    // must fail, rather than serving the old leader's state.
    cluster.RoundTrip(1, 3);
    client.Poll();
    ASSERT_EQ(client.Replies.size(), 1);
    ASSERT_EQ(client.Replies[0].Error, 1);
}

TEST(Correctness, new_read_cannot_reuse_previous_read_quorum) {
    TCluster cluster(5);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    cluster.Replicate(1);
    TReadClient client(cluster.Nodes[0]);
    client.Read(11);
    cluster.RoundTrip(1, 2);
    cluster.RoundTrip(1, 3);
    client.Poll();
    ASSERT_EQ(client.Replies.size(), 1);
    client.Read(22);
    cluster.RoundTrip(1, 2, true);
    client.Poll();
    ASSERT_EQ(client.Replies.size(), 1);
    cluster.RoundTrip(1, 3);
    client.Poll();
    ASSERT_EQ(client.Replies.size(), 2);
    ASSERT_EQ(client.Replies[1].Cookie, 22);
}

TEST(Correctness, ready_read_is_cancelled_on_leadership_change) {
    TCluster cluster(3);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    cluster.Replicate(1);
    TReadClient client(cluster.Nodes[0]);
    client.Read(11);
    cluster.RoundTrip(1, 2);
    // Lose leadership before the application processes reads: node 1 fails, its
    // heartbeat to node 3 is lost and does not hold node 3's vote back.
    cluster.Packets.clear();
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(2));
    client.Poll();
    ASSERT_EQ(client.Replies.size(), 1);
    ASSERT_EQ(client.Replies[0].Error, 1);
}

TEST(Correctness, consistent_flag_takes_precedence_over_stale) {
    TCluster cluster(3);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    cluster.Replicate(1);
    TReadClient client(cluster.Nodes[0]);
    client.Read(11, TCommandRequest::EConsistent | TCommandRequest::EStale);
    client.Poll();
    ASSERT_TRUE(client.Replies.empty());
    cluster.RoundTrip(1, 2);
    client.Poll();
    ASSERT_EQ(client.Replies.size(), 1);
    ASSERT_EQ(client.Replies[0].Error, 0);
}

TEST(Correctness, delayed_pre_read_replies_cannot_confirm_new_read) {
    TCluster cluster(5);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    cluster.Replicate(1);
    cluster.Nodes[0]->ProcessTimeout(cluster.Now += TTimeout::Election);
    for (uint32_t peer = 2; peer <= 5; ++peer) cluster.DeliverOne(1, peer);
    TReadClient client(cluster.Nodes[0]);
    client.Read(11);
    for (uint32_t peer = 2; peer <= 5; ++peer) cluster.DeliverOne(peer, 1);
    client.Poll();
    ASSERT_TRUE(client.Replies.empty());
    cluster.RoundTrip(1, 2, true);
    client.Poll();
    ASSERT_TRUE(client.Replies.empty());
    cluster.RoundTrip(1, 3);
    client.Poll();
    ASSERT_EQ(client.Replies.size(), 1);
}

TEST(Correctness, new_leader_waits_for_current_term_commit_and_application) {
    TCluster cluster(3);
    for (auto& node : cluster.Nodes) {
        node->GetState()->Append(NewHoldedMessage<TLogEntry>());
        const_cast<TVolatileState*>(node->GetVolatileState())->CommitIndex = 1;
    }
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    // First RPCs probe the old prefix; the new-term no-op is not committed yet.
    ASSERT_EQ(cluster.Nodes[0]->GetVolatileState()->CommitIndex, 1);
    ASSERT_EQ(cluster.Nodes[0]->CommitSeqno(), 0);
    TReadClient client(cluster.Nodes[0]);
    client.Read(11);
    client.Poll();
    ASSERT_TRUE(client.Replies.empty());
    cluster.RoundTrip(1, 2);
    ASSERT_EQ(cluster.Nodes[0]->GetVolatileState()->CommitIndex, 2);
    // Fresh confirmation alone is insufficient until the local state machine
    // has applied the read's target prefix.
    client.Processor.ProcessWaiting();
    ASSERT_TRUE(client.Replies.empty());
    client.Poll();
    ASSERT_EQ(client.Replies.size(), 1);
    ASSERT_EQ(client.Replies[0].Error, 0);
}

TEST(Correctness, old_term_read_is_rejected_after_reelection) {
    TCluster cluster(3);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    cluster.Replicate(1);
    TReadClient client(cluster.Nodes[0]);
    client.Read(11);
    // Node 1 fails: the heartbeats of the read are lost and do not hold the votes back.
    cluster.Packets.clear();
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(2));
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    cluster.Replicate(1);
    // No application poll observed the intermediate follower state. The term
    // tag must still reject the old operation before accepting another read.
    client.Read(22);
    ASSERT_EQ(client.Replies.size(), 1);
    ASSERT_EQ(client.Replies[0].Cookie, 11);
    ASSERT_EQ(client.Replies[0].Error, 1);
    cluster.RoundTrip(1, 2);
    client.Poll();
    ASSERT_EQ(client.Replies.size(), 2);
    ASSERT_EQ(client.Replies[1].Cookie, 22);
    ASSERT_EQ(client.Replies[1].Error, 0);
}

TEST(Correctness, majority_all_cluster_sizes) {
    for (int count = 1; count <= 8; ++count) {
        TState state;
        state.Append(NewHoldedMessage<TLogEntry>());
        for (int copies = 1; copies <= count; ++copies) {
            TVolatileState v;
            std::set<uint32_t> voters;
            for (int id = 1; id <= count; ++id) voters.insert(id);
            for (int peer = 1; peer < copies; ++peer) v.MatchIndex[peer + 1] = 1;
            v.CommitAdvance(voters, 1, state);
            ASSERT_EQ(v.CommitIndex, copies >= count / 2 + 1 ? 1 : 0);
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
        ASSERT_EQ(replies.size(), 1);
        ASSERT_EQ(replies[0], 11);
    }
    raft->ProcessTimeout(ITimeSource::Time{});
    processor.ProcessCommitted();
    processor.ProcessWaiting();
    ASSERT_EQ(replies.size(), 2);
    ASSERT_EQ(replies[0], 11);
    ASSERT_EQ(replies[1], 22);
    processor.ProcessWaiting();
    ASSERT_EQ(replies.size(), 2);
}
TEST(Correctness, read_fifo) { ASSERT_NO_FATAL_FAILURE(CheckRequests(0, false)); }
TEST(Correctness, strong_read_fifo) { ASSERT_NO_FATAL_FAILURE(CheckRequests(TCommandRequest::EConsistent, false)); }
TEST(Correctness, write_fifo) { ASSERT_NO_FATAL_FAILURE(CheckRequests(TCommandRequest::EWrite, false)); }
TEST(Correctness, partial_write_completion) { ASSERT_NO_FATAL_FAILURE(CheckRequests(TCommandRequest::EWrite, true)); }

TEST(Correctness, loss_reorder_duplicate_and_recovery) {
    TCluster cluster(3);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    auto index = cluster.Nodes[0]->Append(NewHoldedMessage<TLogEntry>());
    cluster.Nodes[0]->ProcessTimeout(cluster.Now += std::chrono::seconds(1));
    cluster.Packets.clear(); // Drop a complete replication round.
    ASSERT_TRUE(cluster.Nodes[0]->GetVolatileState()->CommitIndex < index);
    cluster.Replicate(1, true, true);
    for (auto& node : cluster.Nodes) ASSERT_EQ(node->GetVolatileState()->CommitIndex, index);
    ASSERT_NO_FATAL_FAILURE(cluster.AssertCommittedPrefix());
}

TEST(Correctness, partition_leader_replacement_and_heal) {
    TCluster cluster(3);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
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
    ASSERT_EQ(cluster.Nodes[0]->GetVolatileState()->CommitIndex, committed);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(2));
    auto replacement = cluster.Nodes[1]->Append(NewHoldedMessage<TLogEntry>());
    cluster.Replicate(2);
    ASSERT_TRUE(cluster.Nodes[1]->GetVolatileState()->CommitIndex >= replacement);
    ASSERT_NO_FATAL_FAILURE(cluster.AssertCommittedPrefix());
    cluster.Isolated.clear();
    // Old-term traffic arrives after the new leader has committed its entry.
    cluster.Packets = std::move(delayed);
    ASSERT_NO_FATAL_FAILURE(cluster.Drain(true, true));
    cluster.Replicate(2, true, true);
    ASSERT_EQ(cluster.Nodes[0]->CurrentStateName(), EState::FOLLOWER);
    for (auto& node : cluster.Nodes) {
        ASSERT_EQ(node->GetVolatileState()->CommitIndex, replacement);
        ASSERT_EQ(node->GetState()->Get(minority)->Flags, TLogEntry::EStub);
    }
    ASSERT_NO_FATAL_FAILURE(cluster.AssertCommittedPrefix());
}

TEST(Correctness, even_cluster_partition_requires_majority) {
    TCluster cluster(4);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    cluster.Replicate(1);
    auto previous = cluster.Nodes[0]->GetVolatileState()->CommitIndex;
    cluster.Isolated = {3, 4};
    auto index = cluster.Nodes[0]->Append(NewHoldedMessage<TLogEntry>());
    cluster.Replicate(1);
    ASSERT_EQ(cluster.Nodes[0]->GetVolatileState()->CommitIndex, previous);
    ASSERT_EQ(cluster.Nodes[1]->GetVolatileState()->CommitIndex, previous);
    cluster.Isolated.clear();
    cluster.Replicate(1, true, true);
    for (auto& node : cluster.Nodes) ASSERT_EQ(node->GetVolatileState()->CommitIndex, index);
    ASSERT_NO_FATAL_FAILURE(cluster.AssertCommittedPrefix());
}

TEST(Correctness, follower_restarted_without_storage_catches_up) {
    TCluster cluster(3);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    for (int i = 0; i < 3; ++i) cluster.Nodes[0]->Append(NewHoldedMessage<TLogEntry>());
    cluster.Replicate(1);
    const auto last = cluster.Nodes[0]->GetState()->LastLogIndex;
    const auto committed = cluster.Nodes[0]->GetVolatileState()->CommitIndex;
    ASSERT_EQ(cluster.Nodes[2]->GetState()->LastLogIndex, last);
    // Node 3 restarts with an empty in-memory log while node 1 stays leader.
    TNodeDict peers;
    for (uint32_t dst : {1u, 2u}) {
        peers[dst] = std::make_shared<TEndpoint>([&cluster](auto m) { cluster.Packets.push_back(std::move(m)); });
    }
    cluster.Nodes[2] = std::make_shared<TRaft>(std::make_shared<TState>(), 3, peers);
    cluster.Replicate(1);
    ASSERT_EQ(cluster.Nodes[0]->CurrentStateName(), EState::LEADER);
    ASSERT_EQ(cluster.Nodes[2]->GetState()->LastLogIndex, last);
    ASSERT_EQ(cluster.Nodes[0]->GetVolatileState()->MatchIndex.at(3), last);
    ASSERT_TRUE(cluster.Nodes[0]->GetVolatileState()->CommitIndex >= committed);
    ASSERT_NO_FATAL_FAILURE(cluster.AssertCommittedPrefix());
}

std::shared_ptr<TEchoRsm> RecoverApplication(const TEchoRsm& old) {
    auto recovered = std::make_shared<TEchoRsm>();
    recovered->Applied = old.Applied;
    recovered->LastAppliedIndex = old.LastAppliedIndex;
    return recovered;
}
TEST(Correctness, recovered_application_progress_is_not_rewound) {
    TCluster cluster(1);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    cluster.Nodes[0]->Append(LogValue("once"));
    cluster.Replicate(1);
    auto original = std::make_shared<TEchoRsm>();
    { TRequestProcessor processor(cluster.Nodes[0], original, {}); processor.ProcessCommitted(); }
    auto recovered = RecoverApplication(*original);
    cluster.Restart(1);
    TRequestProcessor processor(cluster.Nodes[0], recovered, {});
    processor.ProcessCommitted(); // timer before the first post-restart election
    ASSERT_EQ(recovered->LastAppliedIndex, original->LastAppliedIndex);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    processor.ProcessCommitted();
    processor.ProcessCommitted();
    ASSERT_TRUE(recovered->Applied == std::vector<std::string>{"once"});
}
TEST(Correctness, application_progress_never_decreases_on_lower_commit_input) {
    TCluster cluster(1);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
    auto recovered = std::make_shared<TEchoRsm>();
    recovered->LastAppliedIndex = 1;
    TRequestProcessor processor(cluster.Nodes[0], recovered, {});
    // Defensive check even if an embedder violates commit-index monotonicity.
    const_cast<TVolatileState*>(cluster.Nodes[0]->GetVolatileState())->CommitIndex = 0;
    processor.ProcessCommitted();
    ASSERT_EQ(recovered->LastAppliedIndex, 1);
}
TEST(Correctness, recovered_applied_prefix_is_protected_and_suffix_can_change) {
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
    ASSERT_EQ(state->LogTerm(1), 2);
    ASSERT_EQ(cluster.Nodes[0]->GetVolatileState()->CommitIndex, 1);
    cluster.Nodes[0]->Process(cluster.Now, request(1, 2, "replacement"));
    processor.ProcessCommitted();
    ASSERT_TRUE(recovered->Applied == (std::vector<std::string>{"applied", "replacement"}));
    ASSERT_EQ(recovered->LastAppliedIndex, 2);
}
TEST(Correctness, application_ahead_of_recovered_log_is_rejected) {
    TCluster cluster(1);
    for (const auto index : {uint64_t(1), UINT64_MAX}) {
        auto recovered = std::make_shared<TEchoRsm>();
        recovered->LastAppliedIndex = index;
        bool rejected = false;
        try { TRequestProcessor processor(cluster.Nodes[0], recovered, {}); }
        catch (const std::invalid_argument&) { rejected = true; }
        ASSERT_TRUE(rejected);
        ASSERT_TRUE(recovered->LastAppliedIndex == index);
        ASSERT_EQ(cluster.Nodes[0]->GetVolatileState()->CommitIndex, 0);
        ASSERT_EQ(cluster.Nodes[0]->GetLastIndex(), 0);
    }
}
TEST(Correctness, cluster_restart_preserves_applied_history_and_rebuilds_volatile_application) {
    TCluster cluster(3);
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(1));
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
    ASSERT_NO_FATAL_FAILURE(cluster.Elect(2));
    // Recovered old-term commitment cannot replace a fresh current-term read quorum.
    ASSERT_EQ(cluster.Nodes[1]->CommitSeqno(), 0);
    cluster.Nodes[1]->Append(LogValue("second"));
    cluster.Replicate(2, true, true);
    cluster.Packets = std::move(delayed);
    ASSERT_NO_FATAL_FAILURE(cluster.Drain(true, true));
    cluster.Replicate(2, true, true);
    for (auto& processor : processors) { processor->ProcessCommitted(); processor->ProcessCommitted(); }
    for (const auto& rsm : recovered) {
        ASSERT_TRUE(rsm->Applied == (std::vector<std::string>{"first", "second"}));
        ASSERT_EQ(rsm->LastAppliedIndex, cluster.Nodes[1]->GetVolatileState()->CommitIndex);
    }
    ASSERT_NO_FATAL_FAILURE(cluster.AssertCommittedPrefix());
}

}

