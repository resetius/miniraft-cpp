#include <miniraft/raft.h>
#include <functional>
#include <string>
#include <gtest/gtest.h>

namespace {
struct TEndpoint : INode {
    std::function<void(TMessageHolder<TMessage>)> Callback;
    explicit TEndpoint(std::function<void(TMessageHolder<TMessage>)> cb) : Callback(std::move(cb)) {}
    void Send(TMessageHolder<TMessage> message) override { Callback(std::move(message)); }
    void Drain() override {}
};
struct TClient {
    struct TReply { uint32_t Cookie, Error; std::string Data; };
    std::vector<TReply> Replies;
    std::shared_ptr<TEndpoint> Endpoint = std::make_shared<TEndpoint>([this](auto message) {
        auto response = message.template Cast<TCommandResponse>();
        Replies.push_back({response->Cookie, response->ErrorCode,
            std::string(response->Data, response->Len - sizeof(TCommandResponse))});
    });
};
struct TFollower {
    std::vector<TMessageHolder<TCommandRequest>> Sent;
    std::shared_ptr<TEndpoint> Leader = std::make_shared<TEndpoint>([this](auto message) {
        if (message->Type == uint32_t(EMessageType::COMMAND_REQUEST))
            Sent.push_back(message.template Cast<TCommandRequest>());
    });
    std::shared_ptr<TEndpoint> Other = std::make_shared<TEndpoint>([this](auto message) {
        if (message->Type == uint32_t(EMessageType::COMMAND_REQUEST))
            Sent.push_back(message.template Cast<TCommandRequest>());
    });
    TNodeDict Peers{{1, Leader}, {3, Other}};
    std::shared_ptr<TRaft> Raft = std::make_shared<TRaft>(std::make_shared<TState>(), 2, Peers);
    std::shared_ptr<TDummyRsm> Rsm = std::make_shared<TDummyRsm>();
    TRequestProcessor Processor{Raft, Rsm, Peers};
    explicit TFollower(bool known = true) { if (known) LearnLeader(1, 3); }
    void LearnLeader(uint32_t id, uint64_t term) {
        auto message = NewHoldedMessage(TMessageEx{.Src = id, .Dst = 2, .Term = term},
                                       TAppendEntriesRequest{.LeaderId = id});
        Raft->Process(ITimeSource::Time{}, message);
    }
    TMessageHolder<TCommandRequest> Request(TClient& client, uint32_t cookie, uint32_t extra = 0) {
        auto request = NewHoldedMessage<TCommandRequest>(sizeof(TCommandRequest) + 5);
        request->Cookie = cookie;
        request->Flags = TCommandRequest::EWrite | extra;
        memcpy(request->Data, "value", 5);
        Processor.OnCommandRequest(request, client.Endpoint);
        return request;
    }
    TMessageHolder<TCommandResponse> Response(size_t sent, uint32_t error = 0) {
        auto response = NewHoldedMessage<TCommandResponse>(sizeof(TCommandResponse) + 6);
        response->Cookie = Sent.at(sent)->Cookie;
        response->ErrorCode = error;
        memcpy(response->Data, "answer", 6);
        return response;
    }
    void Reply(size_t sent, uint32_t error = 0) {
        Processor.OnCommandResponse(Response(sent, error), Leader);
    }
};

TEST(Forwarding, nonzero_client_cookies_are_forwarded) {
    for (uint32_t cookie : {42u, UINT32_MAX}) {
        TFollower follower;
        TClient client;
        follower.Request(client, cookie);
        ASSERT_EQ(follower.Sent.size(), 1);
        follower.Reply(0);
        ASSERT_EQ(client.Replies.size(), 1);
        ASSERT_EQ(client.Replies[0].Cookie, cookie);
        ASSERT_EQ(client.Replies[0].Error, 0);
    }
}
TEST(Forwarding, zero_client_cookie_is_restored) {
    TFollower follower;
    TClient client;
    follower.Request(client, 0);
    ASSERT_EQ(follower.Sent.size(), 1);
    follower.Reply(0);
    ASSERT_EQ(client.Replies.size(), 1);
    ASSERT_EQ(client.Replies[0].Cookie, 0);
    ASSERT_STREQ(client.Replies[0].Data.c_str(), "answer");
}
TEST(Forwarding, forwarding_copies_request_and_marks_one_hop) {
    TFollower follower;
    TClient client;
    auto original = follower.Request(client, 0);
    ASSERT_EQ(original->Cookie, 0);
    ASSERT_EQ(original->Flags, TCommandRequest::EWrite);
    ASSERT_TRUE(follower.Sent[0]->Flags & TCommandRequest::EForwarded);
    ASSERT_TRUE(follower.Sent[0].RawData != original.RawData);
}
TEST(Forwarding, out_of_order_and_duplicate_replies_preserve_client_routes) {
    TFollower follower;
    TClient first, second;
    follower.Request(first, 0);
    follower.Request(second, 0);
    ASSERT_NE(follower.Sent[0]->Cookie, follower.Sent[1]->Cookie);
    follower.Reply(1);
    follower.Reply(1);
    ASSERT_TRUE(first.Replies.empty());
    ASSERT_EQ(second.Replies.size(), 1);
    ASSERT_EQ(second.Replies[0].Cookie, 0);
    follower.Reply(0);
    ASSERT_EQ(first.Replies.size(), 1);
    ASSERT_EQ(first.Replies[0].Cookie, 0);
}
TEST(Forwarding, forwarded_request_is_rejected_on_term_change) {
    TFollower follower;
    TClient client;
    follower.Request(client, 0);
    follower.LearnLeader(3, 4);
    follower.Processor.CheckStateChange();
    ASSERT_EQ(client.Replies.size(), 1);
    ASSERT_EQ(client.Replies[0].Cookie, 0);
    ASSERT_EQ(client.Replies[0].Error, 1);
    follower.Reply(0);
    ASSERT_EQ(client.Replies.size(), 1);
}
TEST(Forwarding, candidate_transition_rejects_forwarded_requests) {
    TFollower follower;
    TClient client;
    follower.Request(client, 0);
    follower.Raft->ProcessTimeout(ITimeSource::Time{} + std::chrono::seconds(20));
    ASSERT_EQ(follower.Raft->CurrentStateName(), EState::CANDIDATE);
    follower.Processor.CheckStateChange();
    ASSERT_EQ(client.Replies.size(), 1);
    ASSERT_EQ(client.Replies[0].Error, 1);
}
TEST(Forwarding, late_reply_prunes_stale_route_before_application_poll) {
    TFollower follower;
    TClient client;
    follower.Request(client, 0);
    follower.LearnLeader(3, 4);
    follower.Reply(0); // No CheckStateChange/ProcessWaiting in between.
    ASSERT_EQ(client.Replies.size(), 1);
    ASSERT_EQ(client.Replies[0].Error, 1);
    ASSERT_EQ(client.Replies[0].Cookie, 0);
}
TEST(Forwarding, client_disconnect_releases_only_its_forwarded_route) {
    TFollower follower;
    TClient first, second;
    follower.Request(first, 0);
    follower.Request(second, 0);
    std::weak_ptr<INode> endpoint = first.Endpoint;
    follower.Processor.CleanUp(first.Endpoint);
    first.Endpoint.reset();
    ASSERT_TRUE(endpoint.expired());
    follower.Reply(0);
    ASSERT_TRUE(first.Replies.empty());
    follower.Reply(1);
    ASSERT_EQ(second.Replies.size(), 1);
    ASSERT_EQ(second.Replies[0].Error, 0);
}
TEST(Forwarding, already_forwarded_request_is_not_relayed_again) {
    TFollower follower;
    TClient client;
    follower.Request(client, 0, TCommandRequest::EForwarded);
    ASSERT_TRUE(follower.Sent.empty());
    ASSERT_EQ(client.Replies.size(), 1);
    ASSERT_EQ(client.Replies[0].Error, 1);
}
TEST(Forwarding, nonzero_cookie_waits_for_leader_discovery) {
    TFollower follower(false);
    TClient client;
    follower.Request(client, 77);
    ASSERT_TRUE(client.Replies.empty());
    ASSERT_TRUE(follower.Sent.empty());
    follower.LearnLeader(1, 3);
    follower.Processor.CheckStateChange();
    ASSERT_EQ(follower.Sent.size(), 1);
    follower.Reply(0);
    ASSERT_EQ(client.Replies[0].Cookie, 77);
}
TEST(Forwarding, response_requires_original_peer_and_preserves_wire_message) {
    TFollower follower;
    TClient client;
    follower.Request(client, 0);
    auto response = follower.Response(0, 77);
    const auto cookie = response->Cookie;
    follower.Processor.OnCommandResponse(response);
    follower.Processor.OnCommandResponse(response, follower.Other);
    ASSERT_TRUE(client.Replies.empty());
    follower.Processor.OnCommandResponse(response, follower.Leader);
    ASSERT_EQ(client.Replies.size(), 1);
    ASSERT_EQ(client.Replies[0].Cookie, 0);
    ASSERT_EQ(client.Replies[0].Error, 77);
    ASSERT_EQ(response->Cookie, cookie);
}
TEST(Forwarding, peer_disconnect_cancels_once_and_does_not_reuse_cookies) {
    TFollower follower;
    TClient first, second;
    follower.Request(first, 42);
    follower.Request(second, 43);
    follower.Processor.OnPeerDisconnected(follower.Other);
    ASSERT_TRUE(first.Replies.empty());
    ASSERT_TRUE(second.Replies.empty());
    follower.Processor.OnPeerDisconnected(follower.Leader);
    follower.Processor.OnPeerDisconnected(follower.Leader);
    ASSERT_EQ(first.Replies.size(), 1);
    ASSERT_EQ(second.Replies.size(), 1);
    ASSERT_EQ(first.Replies[0].Cookie, 42);
    ASSERT_EQ(second.Replies[0].Cookie, 43);
    ASSERT_EQ(first.Replies[0].Error, 1);
    ASSERT_EQ(second.Replies[0].Error, 1);
    std::weak_ptr<INode> endpoint = first.Endpoint;
    first.Endpoint.reset();
    ASSERT_TRUE(endpoint.expired());
    TClient afterReconnect;
    follower.Request(afterReconnect, 44);
    ASSERT_TRUE(follower.Sent[2]->Cookie > follower.Sent[1]->Cookie);
    follower.Reply(0); // Late old-connection reply cannot redeem the new route.
    ASSERT_TRUE(afterReconnect.Replies.empty());
    follower.Reply(2);
    ASSERT_EQ(afterReconnect.Replies.size(), 1);
    ASSERT_EQ(afterReconnect.Replies[0].Cookie, 44);
    ASSERT_EQ(afterReconnect.Replies[0].Error, 0);
}
}

