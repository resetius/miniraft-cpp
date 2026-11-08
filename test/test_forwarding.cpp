#include <miniraft/raft.h>
#include <functional>
#include <string>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
extern "C" {
#include <cmocka.h>
}

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

void test_nonzero_client_cookies_are_forwarded(void**) {
    for (uint32_t cookie : {42u, UINT32_MAX}) {
        TFollower follower;
        TClient client;
        follower.Request(client, cookie);
        assert_int_equal(follower.Sent.size(), 1);
        follower.Reply(0);
        assert_int_equal(client.Replies.size(), 1);
        assert_int_equal(client.Replies[0].Cookie, cookie);
        assert_int_equal(client.Replies[0].Error, 0);
    }
}
void test_zero_client_cookie_is_restored(void**) {
    TFollower follower;
    TClient client;
    follower.Request(client, 0);
    assert_int_equal(follower.Sent.size(), 1);
    follower.Reply(0);
    assert_int_equal(client.Replies.size(), 1);
    assert_int_equal(client.Replies[0].Cookie, 0);
    assert_string_equal(client.Replies[0].Data.c_str(), "answer");
}
void test_forwarding_copies_request_and_marks_one_hop(void**) {
    TFollower follower;
    TClient client;
    auto original = follower.Request(client, 0);
    assert_int_equal(original->Cookie, 0);
    assert_int_equal(original->Flags, TCommandRequest::EWrite);
    assert_true(follower.Sent[0]->Flags & TCommandRequest::EForwarded);
    assert_true(follower.Sent[0].RawData != original.RawData);
}
void test_out_of_order_and_duplicate_replies_preserve_client_routes(void**) {
    TFollower follower;
    TClient first, second;
    follower.Request(first, 0);
    follower.Request(second, 0);
    assert_int_not_equal(follower.Sent[0]->Cookie, follower.Sent[1]->Cookie);
    follower.Reply(1);
    follower.Reply(1);
    assert_true(first.Replies.empty());
    assert_int_equal(second.Replies.size(), 1);
    assert_int_equal(second.Replies[0].Cookie, 0);
    follower.Reply(0);
    assert_int_equal(first.Replies.size(), 1);
    assert_int_equal(first.Replies[0].Cookie, 0);
}
void test_forwarded_request_is_rejected_on_term_change(void**) {
    TFollower follower;
    TClient client;
    follower.Request(client, 0);
    follower.LearnLeader(3, 4);
    follower.Processor.CheckStateChange();
    assert_int_equal(client.Replies.size(), 1);
    assert_int_equal(client.Replies[0].Cookie, 0);
    assert_int_equal(client.Replies[0].Error, 1);
    follower.Reply(0);
    assert_int_equal(client.Replies.size(), 1);
}
void test_candidate_transition_rejects_forwarded_requests(void**) {
    TFollower follower;
    TClient client;
    follower.Request(client, 0);
    follower.Raft->ProcessTimeout(ITimeSource::Time{} + std::chrono::seconds(20));
    assert_int_equal(follower.Raft->CurrentStateName(), EState::CANDIDATE);
    follower.Processor.CheckStateChange();
    assert_int_equal(client.Replies.size(), 1);
    assert_int_equal(client.Replies[0].Error, 1);
}
void test_late_reply_prunes_stale_route_before_application_poll(void**) {
    TFollower follower;
    TClient client;
    follower.Request(client, 0);
    follower.LearnLeader(3, 4);
    follower.Reply(0); // No CheckStateChange/ProcessWaiting in between.
    assert_int_equal(client.Replies.size(), 1);
    assert_int_equal(client.Replies[0].Error, 1);
    assert_int_equal(client.Replies[0].Cookie, 0);
}
void test_client_disconnect_releases_only_its_forwarded_route(void**) {
    TFollower follower;
    TClient first, second;
    follower.Request(first, 0);
    follower.Request(second, 0);
    std::weak_ptr<INode> endpoint = first.Endpoint;
    follower.Processor.CleanUp(first.Endpoint);
    first.Endpoint.reset();
    assert_true(endpoint.expired());
    follower.Reply(0);
    assert_true(first.Replies.empty());
    follower.Reply(1);
    assert_int_equal(second.Replies.size(), 1);
    assert_int_equal(second.Replies[0].Error, 0);
}
void test_already_forwarded_request_is_not_relayed_again(void**) {
    TFollower follower;
    TClient client;
    follower.Request(client, 0, TCommandRequest::EForwarded);
    assert_true(follower.Sent.empty());
    assert_int_equal(client.Replies.size(), 1);
    assert_int_equal(client.Replies[0].Error, 1);
}
void test_nonzero_cookie_waits_for_leader_discovery(void**) {
    TFollower follower(false);
    TClient client;
    follower.Request(client, 77);
    assert_true(client.Replies.empty());
    assert_true(follower.Sent.empty());
    follower.LearnLeader(1, 3);
    follower.Processor.CheckStateChange();
    assert_int_equal(follower.Sent.size(), 1);
    follower.Reply(0);
    assert_int_equal(client.Replies[0].Cookie, 77);
}
void test_response_requires_original_peer_and_preserves_wire_message(void**) {
    TFollower follower;
    TClient client;
    follower.Request(client, 0);
    auto response = follower.Response(0, 77);
    const auto cookie = response->Cookie;
    follower.Processor.OnCommandResponse(response);
    follower.Processor.OnCommandResponse(response, follower.Other);
    assert_true(client.Replies.empty());
    follower.Processor.OnCommandResponse(response, follower.Leader);
    assert_int_equal(client.Replies.size(), 1);
    assert_int_equal(client.Replies[0].Cookie, 0);
    assert_int_equal(client.Replies[0].Error, 77);
    assert_int_equal(response->Cookie, cookie);
}
void test_peer_disconnect_cancels_once_and_does_not_reuse_cookies(void**) {
    TFollower follower;
    TClient first, second;
    follower.Request(first, 42);
    follower.Request(second, 43);
    follower.Processor.OnPeerDisconnected(follower.Other);
    assert_true(first.Replies.empty());
    assert_true(second.Replies.empty());
    follower.Processor.OnPeerDisconnected(follower.Leader);
    follower.Processor.OnPeerDisconnected(follower.Leader);
    assert_int_equal(first.Replies.size(), 1);
    assert_int_equal(second.Replies.size(), 1);
    assert_int_equal(first.Replies[0].Cookie, 42);
    assert_int_equal(second.Replies[0].Cookie, 43);
    assert_int_equal(first.Replies[0].Error, 1);
    assert_int_equal(second.Replies[0].Error, 1);
    std::weak_ptr<INode> endpoint = first.Endpoint;
    first.Endpoint.reset();
    assert_true(endpoint.expired());
    TClient afterReconnect;
    follower.Request(afterReconnect, 44);
    assert_true(follower.Sent[2]->Cookie > follower.Sent[1]->Cookie);
    follower.Reply(0); // Late old-connection reply cannot redeem the new route.
    assert_true(afterReconnect.Replies.empty());
    follower.Reply(2);
    assert_int_equal(afterReconnect.Replies.size(), 1);
    assert_int_equal(afterReconnect.Replies[0].Cookie, 44);
    assert_int_equal(afterReconnect.Replies[0].Error, 0);
}
}

int main() {
    const CMUnitTest tests[] = {
        cmocka_unit_test(test_nonzero_client_cookies_are_forwarded),
        cmocka_unit_test(test_zero_client_cookie_is_restored),
        cmocka_unit_test(test_forwarding_copies_request_and_marks_one_hop),
        cmocka_unit_test(test_out_of_order_and_duplicate_replies_preserve_client_routes),
        cmocka_unit_test(test_forwarded_request_is_rejected_on_term_change),
        cmocka_unit_test(test_candidate_transition_rejects_forwarded_requests),
        cmocka_unit_test(test_late_reply_prunes_stale_route_before_application_poll),
        cmocka_unit_test(test_client_disconnect_releases_only_its_forwarded_route),
        cmocka_unit_test(test_already_forwarded_request_is_not_relayed_again),
        cmocka_unit_test(test_nonzero_cookie_waits_for_leader_discovery),
        cmocka_unit_test(test_response_requires_original_peer_and_preserves_wire_message),
        cmocka_unit_test(test_peer_disconnect_cancels_once_and_does_not_reuse_cookies),
    };
    return cmocka_run_group_tests(tests, nullptr, nullptr);
}
