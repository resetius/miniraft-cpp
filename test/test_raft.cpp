#include <chrono>
#include <coroutine>
#include <cstdint>
#include <iostream>
#include <memory>
#include <functional>

#include <messages.h>
#include <raft.h>
#include <timesource.h>
#include <coroio/all.hpp>

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
extern "C" {
#include <cmocka.h>
}

using namespace NNet;

namespace {

using OnSendFunc = const std::function<void(TMessageHolder<TMessage>)>;

class TFakeNode: public INode {
public:
    TFakeNode(const OnSendFunc& sendFunc = {})
        : SendFunc(sendFunc)
    { }

    void Send(TMessageHolder<TMessage> message) override {
        if (SendFunc) {
            SendFunc(std::move(message));
        }
    }

    void Drain() override { }

private:
    OnSendFunc SendFunc;
};

class TFakeTimeSource: public ITimeSource {
public:
    TFakeTimeSource()
        : T(std::chrono::steady_clock::now())
    { }

    ITimeSource::Time Now() override {
        return T;
    }

    template<typename A, typename B>
    void Advance(std::chrono::duration<A, B> duration) {
        T += duration;
    }

private:
    ITimeSource::Time T;
};

std::shared_ptr<TRaft> MakeRaft(
    const OnSendFunc& sendFunc = {},
    int count = 3,
    TState st = {})
{
    std::shared_ptr<IRsm> rsm = std::make_shared<TDummyRsm>();
    TNodeDict nodes;
    for (int i = 2; i <= count; i++) {
        nodes[i] = std::make_shared<TFakeNode>(sendFunc);
    }
    std::shared_ptr<IState> state = std::make_shared<TState>(st);
    return std::make_shared<TRaft>(std::move(rsm), std::move(state), 1, nodes);
}

template<typename T=TMessage>
std::vector<TMessageHolder<T>> MakeLog(const std::vector<uint64_t>& terms) {
    std::vector<TMessageHolder<T>> entries;
    for (auto term : terms) {
        auto mes = NewHoldedMessage<TLogEntry>();
        mes->Term = term;
        entries.push_back(mes);
    }
    return entries;
}

template<typename T>
void SetPayload(TMessageHolder<T>& dst, const std::vector<TMessageHolder<TMessage>>& payload) {
    dst.InitPayload(payload.size());
    for (uint32_t i = 0; i < payload.size(); ++i) {
        dst.Payload[i] = payload[i];
    }
}

void assert_terms(const std::vector<TMessageHolder<TLogEntry>>& log, const std::vector<uint64_t>& terms)
{
    assert_int_equal(log.size(), terms.size());
    for (size_t i = 0; i < log.size(); i++) {
        assert_int_equal(log[i]->Term, terms[i]);
    }
}

template<typename T>
void assert_message_equal(TMessageHolder<TMessage> m1, const T& m2) {
    auto mm = m1.Maybe<T>();
    assert(mm);
    auto m = mm.Cast();
    if (memcmp(m.Mes, &m2, sizeof(T)) != 0) {
        const char* p1 = (const char*)m.Mes;
        const char* p2 = (const char*)&m2;
        assert_memory_equal(p1, p2, sizeof(m2));
    }
}

} // namespace {

void test_empty(void** state) {

}

void test_message_create(void** state) {
    auto mes = NewHoldedMessage<TMessage>(0, 8);
    assert_true(mes->Len == 8);
    assert_true(mes->Type == 0);
}

void test_message_cast(void** state) {
    TMessageHolder<TMessage> mes = NewHoldedMessage<TLogEntry>(16);
    auto casted = mes.Cast<TLogEntry>();
    assert_true(mes.RawData == casted.RawData);
    assert_true(mes->Len == casted->Len);

    auto casted2 = mes.Maybe<TRequestVoteRequest>();
    assert_false(casted2);

    auto maybeCasted = mes.Maybe<TLogEntry>();
    assert_true(maybeCasted);
    auto casted3 = maybeCasted.Cast();
    assert_true(mes.RawData == casted3.RawData);
    assert_true(mes->Len == casted3->Len);
}

void test_message_send_recv(void** state) {
    const char* text = "MESSAGE";
    auto mes = NewHoldedMessage<TLogEntry>(
        static_cast<uint32_t>(TLogEntry::MessageType),
        sizeof(TLogEntry) + strlen(text) + 1
    );
    strcpy(mes.Mes->Data, text);

    TLoop<TPoll> loop;
    TSocket socket(TAddress{"127.0.0.1", 8888}, loop.Poller());
    socket.Bind();
    socket.Listen();

    TSocket client(TAddress{"127.0.0.1", 8888}, loop.Poller());

    TVoidSuspendedTask h1 = [](TSocket& client, TMessageHolder<TLogEntry> mes) -> TVoidSuspendedTask
    {
        co_await client.Connect();
        auto r = co_await client.WriteSome(mes.RawData.get(), mes->Len);
        co_return;
    }(client, mes);

    TMessageHolder<TMessage> received;
    TVoidSuspendedTask h2 = [](TSocket& server, TMessageHolder<TMessage>& received) -> TVoidSuspendedTask
    {
        auto client = std::move(co_await server.Accept());
        uint32_t type, len;
        auto r = co_await client.ReadSome(&type, sizeof(type));
        r = co_await client.ReadSome(&len, sizeof(len));
        received = NewHoldedMessage<TMessage>(type, len);
        r = co_await client.ReadSome(received->Value, len - sizeof(TMessage));
        co_return;
    }(socket, received);

    while (!(h1.done() && h2.done())) {
        loop.Step();
    }

    auto maybeCasted = received.Maybe<TLogEntry>();
    assert_true(maybeCasted);
    auto casted = maybeCasted.Cast();
    assert_string_equal(mes->Data, casted->Data);

    h1.destroy(); h2.destroy();
};

void test_initial(void**) {
    auto raft = MakeRaft();
    assert_true(raft->CurrentStateName() == EState::FOLLOWER);
}

void test_numbers(void**) {
    auto raft = MakeRaft({}, 3);
    assert_int_equal(raft->GetMinVotes(), 2);
    assert_int_equal(raft->GetNservers(), 3);
    assert_int_equal(raft->GetNpeers(), 2);

    raft = MakeRaft({}, 2);
    assert_int_equal(raft->GetMinVotes(), 2);
    assert_int_equal(raft->GetNservers(), 2);
    assert_int_equal(raft->GetNpeers(), 1);

    raft = MakeRaft({}, 1);
    assert_int_equal(raft->GetMinVotes(), 1);
    assert_int_equal(raft->GetNservers(), 1);
    assert_int_equal(raft->GetNpeers(), 0);

    raft = MakeRaft({}, 5);
    assert_int_equal(raft->GetMinVotes(), 3);
    assert_int_equal(raft->GetNservers(), 5);
    assert_int_equal(raft->GetNpeers(), 4);

    raft = MakeRaft({}, 10);
    assert_int_equal(raft->GetMinVotes(), 6);
    assert_int_equal(raft->GetNservers(), 10);
    assert_int_equal(raft->GetNpeers(), 9);
}

void test_become(void**) {
    auto raft = MakeRaft();
    assert_true(raft->CurrentStateName() == EState::FOLLOWER);
    raft->Become(EState::CANDIDATE);
    assert_true(raft->CurrentStateName() == EState::CANDIDATE);
}

void test_become_same_func(void**) {
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft({}, 3);
    assert_true(raft->CurrentStateName() == EState::FOLLOWER);
    ts->Advance(std::chrono::milliseconds(10000));
    raft->Become(EState::FOLLOWER);
    assert_true(raft->CurrentStateName() == EState::FOLLOWER);
}

void test_follower_to_candidate_on_timeout(void**) {
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft({}, 3);
    assert_true(raft->CurrentStateName() == EState::FOLLOWER);
    ts->Advance(std::chrono::milliseconds(10000));
    raft->ProcessTimeout(ts->Now());
    assert_true(raft->CurrentStateName() == EState::CANDIDATE);
}

void test_follower_append_entries_small_term(void**) {
    std::vector<TMessageHolder<TMessage>> messages;
    auto onSend = [&](const TMessageHolder<TMessage>& message) {
        messages.push_back(message);
    };
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft(onSend, 3);
    auto mes = NewHoldedMessage(TMessageEx {
        .Src = 2,
        .Dst = 1,
        .Term = 0,
    }, TAppendEntriesRequest {
        .PrevLogIndex = 0,
        .PrevLogTerm = 0,
        .LeaderCommit = 0,
        .LeaderId = 2,
        .Nentries = 0,
    });
    raft->Process(ts->Now(), mes);

    assert_true(messages.size() == 1);
    auto maybeReply = messages[0].Maybe<TAppendEntriesResponse>();
    assert_true(maybeReply);
    auto reply = maybeReply.Cast();
    assert_true(reply->Dst == 2);
    assert_false(reply->Success);
}

void test_follower_append_entries_7a(void**) {
    // leader: 1,1,1,4,4,5,5,6,6,6
    std::vector<TMessageHolder<TMessage>> messages;
    auto onSend = [&](const TMessageHolder<TMessage>& message) {
        messages.push_back(message);
    };
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft(onSend, 3, TState{
        /*.CurrentTerm =*/ 1,
        /*.VotedFor =*/ 2,
        /*.Log =*/ MakeLog<TLogEntry>({1,1,1,4,4,5,5,6,6})
    });
    auto mes = NewHoldedMessage(TMessageEx {
        .Src = 2,
        .Dst = 1,
        .Term = 1,
    }, TAppendEntriesRequest {
        .PrevLogIndex = 9,
        .PrevLogTerm = 6,
        .LeaderCommit = 9,
        .LeaderId = 2,
        .Nentries = 1,
    });
    SetPayload(mes, MakeLog({6}));
    raft->Process(ts->Now(), mes);
    assert_int_equal(messages.size(), 1);
    assert_true(messages.back().Maybe<TAppendEntriesResponse>());
    auto last = messages.back().Cast<TAppendEntriesResponse>();
    assert_true(last->Success);
    assert_true(last->MatchIndex = 10);
    assert_true(raft->GetState()->LastLogIndex == 10);
}

void test_follower_append_entries_7b(void**) {
    // leader: 1,1,1,4,4,5,5,6,6,6
    std::vector<TMessageHolder<TMessage>> messages;
    auto onSend = [&](const TMessageHolder<TMessage>& message) {
        messages.push_back(message);
    };
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft(onSend, 3, TState{
        /*.CurrentTerm =*/ 1,
        /*.VotedFor =*/ 2,
        /*.Log =*/ MakeLog<TLogEntry>({1,1,1,4})
    });
    auto mes = NewHoldedMessage(TMessageEx {
        .Src = 2,
        .Dst = 1,
        .Term = 1,
    }, TAppendEntriesRequest {
        .PrevLogIndex = 4,
        .PrevLogTerm = 4,
        .LeaderCommit = 9,
        .LeaderId = 2,
        .Nentries = 6,
    });
    SetPayload(mes, MakeLog({4,5,5,6,6,6}));
    raft->Process(ts->Now(), mes);
    assert_int_equal(messages.size(), 1);
    assert_true(messages.back().Maybe<TAppendEntriesResponse>());
    auto last = messages.back().Cast<TAppendEntriesResponse>();
    assert_true(last->Success);
    assert_true(last->MatchIndex = 10);
    assert_true(raft->GetState()->LastLogIndex == 10);
}

void test_follower_append_entries_7c(void**) {
    // leader: 1,1,1,4,4,5,5,6,6,6
    std::vector<TMessageHolder<TMessage>> messages;
    auto onSend = [&](const TMessageHolder<TMessage>& message) {
        messages.push_back(message);
    };
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft(onSend, 3, TState{
        /*.CurrentTerm =*/ 1,
        /*.VotedFor =*/ 2,
        /*.Log =*/ MakeLog<TLogEntry>({1,1,1,4,4,5,5,6,6,6,6})
    });
    auto mes = NewHoldedMessage(TMessageEx {
        .Src = 2,
        .Dst = 1,
        .Term = 1,
    }, TAppendEntriesRequest {
        .PrevLogIndex = 9,
        .PrevLogTerm = 6,
        .LeaderCommit = 9,
        .LeaderId = 2,
        .Nentries = 1,
    });
    SetPayload(mes, MakeLog({6}));
    raft->Process(ts->Now(), mes);
    assert_int_equal(messages.size(), 1);
    assert_true(messages.back().Maybe<TAppendEntriesResponse>());
    auto last = messages.back().Cast<TAppendEntriesResponse>();
    assert_true(last->Success);
    assert_true(last->MatchIndex = 10);
    assert_true(raft->GetState()->LastLogIndex == 11);
}

void test_follower_append_entries_7f(void**) {
    // leader: 1,1,1,4,4,5,5,6,6,6
    std::vector<TMessageHolder<TMessage>> messages;
    auto onSend = [&](const TMessageHolder<TMessage>& message) {
        messages.push_back(message);
    };
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft(onSend, 3, TState{
        /*.CurrentTerm =*/ 1,
        /*.VotedFor =*/ 2,
        /*.Log =*/ MakeLog<TLogEntry>({1,1,1,2,2,2,3,3,3,3,3})
    });
    auto mes = NewHoldedMessage(TMessageEx {
        .Src = 2,
        .Dst = 1,
        .Term = 8,
    }, TAppendEntriesRequest {
        .PrevLogIndex = 3,
        .PrevLogTerm = 1,
        .LeaderCommit = 9,
        .LeaderId = 2,
        .Nentries = 7,
    });
    SetPayload(mes, MakeLog({4,4,5,5,6,6,6}));
    raft->Process(ts->Now(), mes);
    assert_int_equal(messages.size(), 1);
    assert_true(messages.back().Maybe<TAppendEntriesResponse>());
    auto last = messages.back().Cast<TAppendEntriesResponse>();
    assert_true(last->Success);
    assert_true(last->MatchIndex = 10);
    assert_true(raft->GetState()->LastLogIndex == 10);
    assert_terms(std::static_pointer_cast<TState>(raft->GetState())->Log, {1,1,1,4,4,5,5,6,6,6});
}

void test_follower_append_entries_empty_to_empty_log(void**) {
    std::vector<TMessageHolder<TMessage>> messages;
    auto onSend = [&](auto message) {
        messages.emplace_back(std::move(message));
    };
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft(onSend, 3);
    auto mes = NewHoldedMessage(TMessageEx {
        .Src = 2,
        .Dst = 1,
        .Term = 1,
    }, TAppendEntriesRequest {
        .PrevLogIndex = 0,
        .PrevLogTerm = 0,
        .LeaderCommit = 0,
        .LeaderId = 2,
        .Nentries = 0,
    });
    raft->Process(ts->Now(), mes);
    assert_int_equal(messages.size(), 1);
    assert_true(messages.back().Maybe<TAppendEntriesResponse>());
    auto last = messages.back().Cast<TAppendEntriesResponse>();
    assert_int_equal(last->Dst, 2);
    assert_true(last->Success);
    assert_int_equal(last->MatchIndex, 0);
}

void test_candidate_initiate_election(void**) {
    std::vector<TMessageHolder<TRequestVoteResponse>> messages;
    auto onSend = [&](auto message) {
        messages.emplace_back(message.template Cast<TRequestVoteResponse>());
    };
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft(onSend, 3);
    ts->Advance(std::chrono::milliseconds(10000));
    auto term = raft->GetState()->CurrentTerm;
    raft->Become(EState::CANDIDATE);
    raft->ProcessTimeout(ts->Now());
    assert_int_equal(raft->GetState()->CurrentTerm, term+1);
    assert_int_equal(messages.size(), 2);
    auto r = NewHoldedMessage(TMessageEx {
        .Src = 1,
        .Dst = 0,
        .Term = term+1,
    }, TRequestVoteRequest {
        .LastLogIndex = 0,
        .LastLogTerm = 0,
        .CandidateId = raft->GetId(),
    });
    messages[0]->Dst = 0;
    assert_message_equal(messages[0], *r.Mes);
    messages[1]->Dst = 0;
    assert_message_equal(messages[1], *r.Mes);
}

void test_candidate_vote_request_small_term(void**) {
    std::vector<TMessageHolder<TMessage>> messages;
    auto onSend = [&](auto message) {
        messages.emplace_back(std::move(message));
    };

    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft(onSend, 3);
    auto req = NewHoldedMessage(TMessageEx {
        .Src = 2,
        .Dst = 1,
        .Term = 0,
    }, TRequestVoteRequest {
        .LastLogIndex = 1,
        .LastLogTerm = 1,
        .CandidateId = 2,
    });
    raft->Process(ts->Now(), std::move(req));
    auto res = NewHoldedMessage(TMessageEx {
        .Src = 1,
        .Dst = 2,
        .Term = raft->GetState()->CurrentTerm,
    }, TRequestVoteResponse {
        .VoteGranted = false,
    });
    assert_int_equal(messages.size(), 1);
    assert_message_equal(messages[0], *res.Mes);
    assert_int_equal(raft->GetState()->CurrentTerm, 1);
}

void test_candidate_vote_request_ok_term(void**) {
    std::vector<TMessageHolder<TMessage>> messages;
    auto onSend = [&](auto message) {
        messages.emplace_back(std::move(message));
    };

    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft(onSend, 3);
    auto req = NewHoldedMessage(TMessageEx {
        .Src = 2,
        .Dst = 1,
        .Term = 1,
    }, TRequestVoteRequest {
        .LastLogIndex = 1,
        .LastLogTerm = 1,
        .CandidateId = 2,
    });
    raft->Process(ts->Now(), std::move(req));
    auto res = NewHoldedMessage(TMessageEx {
        .Src = 1,
        .Dst = 2,
        .Term = raft->GetState()->CurrentTerm,
    }, TRequestVoteResponse {
        .VoteGranted = true,
    });
    assert_int_equal(messages.size(), 1);
    assert_message_equal(messages[0], *res.Mes);
    assert_int_equal(raft->GetState()->CurrentTerm, 1);
}

void test_candidate_vote_request_big(void**) {
    auto raft = MakeRaft();
    auto ts = std::make_shared<TFakeTimeSource>();
    raft->Become(EState::CANDIDATE);
    auto req = NewHoldedMessage(TMessageEx {
        .Src = 2,
        .Dst = 1,
        .Term = 3,
    }, TRequestVoteRequest {
        .LastLogIndex = 1,
        .LastLogTerm = 1,
        .CandidateId = 2,
    });
    raft->Process(ts->Now(), req);
    assert_true(raft->CurrentStateName() == EState::FOLLOWER);
}

void test_candidate_vote_after_start(void**) {
    std::vector<TMessageHolder<TMessage>> messages;
    auto onSend = [&](auto message) {
        messages.emplace_back(std::move(message));
    };
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft(onSend, 3);
    assert_true(raft->CurrentStateName() == EState::FOLLOWER);
    ts->Advance(std::chrono::milliseconds(10000));
    raft->Become(EState::CANDIDATE);
    raft->ProcessTimeout(ts->Now());
    assert_int_equal(raft->GetState()->VotedFor, 1);
    assert_int_equal(raft->GetState()->CurrentTerm, 2);
    auto req = NewHoldedMessage(TMessageEx {
        .Src = 2,
        .Dst = 1,
        .Term = 2,
    }, TRequestVoteRequest {
        .LastLogIndex = 1,
        .LastLogTerm = 1,
        .CandidateId = 2,
    });
    raft->Process(ts->Now(), req);
    auto last = messages.back().Cast<TRequestVoteResponse>();
    assert_int_equal(last->VoteGranted, false);

    // request with higher term => follower
    req = NewHoldedMessage(TMessageEx {
        .Src = 2,
        .Dst = 1,
        .Term = 3,
    }, TRequestVoteRequest {
        .LastLogIndex = 1,
        .LastLogTerm = 1,
        .CandidateId = 3,
    });
    raft->Process(ts->Now(), req);
    last = messages.back().Cast<TRequestVoteResponse>();
    assert_int_equal(raft->GetState()->VotedFor, 3);
    assert_int_equal(last->VoteGranted, true);
}

void test_election_5_nodes(void**) {
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft({}, 5);
    ts->Advance(std::chrono::milliseconds(10000));
    raft->Become(EState::CANDIDATE);
    auto req = NewHoldedMessage(TMessageEx {
        .Src = 2,
        .Dst = 1,
        .Term = 2,
    }, TRequestVoteResponse {
        .VoteGranted = true,
    });
    ts->Advance(std::chrono::milliseconds(10000));
    raft->ProcessTimeout(ts->Now());

    raft->Process(ts->Now(), req);

    assert_int_equal(raft->CurrentStateName(), EState::CANDIDATE);
    req->Src = 2;
    raft->Process(ts->Now(), req);
    raft->ProcessTimeout(ts->Now());
    assert_int_equal(raft->CurrentStateName(), EState::CANDIDATE);

    req->Src = 4;
    raft->Process(ts->Now(), req);
    raft->ProcessTimeout(ts->Now());
    assert_int_equal(raft->CurrentStateName(), EState::LEADER);
}

void test_commit_advance(void**) {
    auto state = TState {
        /*.CurrentTerm =*/ 1,
        0,
        /*.Log =*/ MakeLog<TLogEntry>({1})
    };

    auto s = TVolatileState {
        .MatchIndex = {{1, 1}}
    };

    auto s1 = TVolatileState(s).CommitAdvance(3, state);
    assert_int_equal(s1.CommitIndex, 1);
    s1 = TVolatileState(s).CommitAdvance(5, state);
    assert_int_equal(s1.CommitIndex, 0);

    s = TVolatileState {
        .MatchIndex = {{1, 1}, {2, 2}}
    };
    auto add = MakeLog<TLogEntry>({1});
    state.Log.insert(state.Log.end(), add.begin(), add.end());
    s1 = TVolatileState(s).CommitAdvance(3, state);
    assert_int_equal(s1.CommitIndex, 2);
    s1 = TVolatileState(s).CommitAdvance(5, state);
    assert_int_equal(s1.CommitIndex, 1);
}

void test_commit_advance_wrong_term(void**) {
    auto state = TState {
        /*.CurrentTerm =*/ 2,
        0,
        /*.Log =*/ MakeLog<TLogEntry>({1,1})
    };
    auto s = TVolatileState {
        .MatchIndex = {{1, 1}, {2, 2}}
    };
    auto s1 = TVolatileState(s).CommitAdvance(3, state);
    assert_int_equal(s1.CommitIndex, 0);
}

void test_leader_heartbeat(void**) {
    std::vector<TMessageHolder<TMessage>> messages;
    auto onSend = [&](auto message) {
        messages.emplace_back(std::move(message));
    };
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft(onSend, 3);
    ts->Advance(std::chrono::milliseconds(10000));
    raft->Become(EState::LEADER);
    raft->ProcessTimeout(ts->Now());
    assert_int_equal(messages.size(), 2);
    auto maybeReq0 = messages[0].Maybe<TAppendEntriesRequest>();
    auto maybeReq1 = messages[1].Maybe<TAppendEntriesRequest>();
    assert_true(maybeReq0);
    assert_true(maybeReq1);
    auto req0 = maybeReq0.Cast();
    auto req1 = maybeReq1.Cast();
    assert_int_equal(req0->Src, 1);
    assert_int_equal(req1->Src, 1);
    assert_true((req0->Dst == 2 && req1->Dst == 3) || (req0->Dst == 3 && req1->Dst == 2));
    assert_int_equal(req0->Nentries, 0);
    assert_int_equal(req1->Nentries, 0);
}

int main() {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_empty),
        cmocka_unit_test(test_message_create),
        cmocka_unit_test(test_message_cast),
        cmocka_unit_test(test_message_send_recv),
        cmocka_unit_test(test_initial),
        cmocka_unit_test(test_numbers),
        cmocka_unit_test(test_become),
        cmocka_unit_test(test_become_same_func),
        cmocka_unit_test(test_follower_to_candidate_on_timeout),
        cmocka_unit_test(test_follower_append_entries_small_term),
        cmocka_unit_test(test_follower_append_entries_7a),
        cmocka_unit_test(test_follower_append_entries_7b),
        cmocka_unit_test(test_follower_append_entries_7c),
        cmocka_unit_test(test_follower_append_entries_7f),
        cmocka_unit_test(test_follower_append_entries_empty_to_empty_log),
        cmocka_unit_test(test_candidate_initiate_election),
        cmocka_unit_test(test_candidate_vote_request_small_term),
        cmocka_unit_test(test_candidate_vote_request_ok_term),
        cmocka_unit_test(test_candidate_vote_request_big),
        cmocka_unit_test(test_candidate_vote_after_start),
        cmocka_unit_test(test_election_5_nodes),
        cmocka_unit_test(test_commit_advance),
        cmocka_unit_test(test_commit_advance_wrong_term),
        cmocka_unit_test(test_leader_heartbeat),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
