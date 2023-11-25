#include <chrono>
#include <coroutine>
#include <cstdint>
#include <iostream>
#include <memory>
#include <functional>

#include <poll.hpp>
#include <messages.h>
#include <raft.h>
#include <timesource.h>
#include <all.hpp>

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
extern "C" {
#include <cmocka.h>
}

using namespace NNet;

namespace {

using OnSendFunc = const std::function<void(const TMessageHolder<TMessage>&)>;

class TFakeNode: public INode {
public:
    TFakeNode(const OnSendFunc& sendFunc = {})
        : SendFunc(sendFunc)
    { }

    void Send(const TMessageHolder<TMessage>& message) override {
        if (SendFunc) {
            SendFunc(message);
        }
    }

private:
    OnSendFunc SendFunc;
};

class TFakeTimeSource: public ITimeSource {
public:
    TFakeTimeSource()
        : T(std::chrono::system_clock::now())
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
    const std::shared_ptr<ITimeSource>& timeSource = std::make_shared<TFakeTimeSource>())
{
    TNodeDict nodes;
    for (int i = 2; i <= count; i++) {
        nodes[i] = std::make_shared<TFakeNode>(sendFunc);
    }
    return std::make_shared<TRaft>(1, nodes, timeSource);
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
    auto mes = NewHoldedMessage<TMessage>(0, 4);
    assert_true(mes->Len == 4);
    assert_true(mes->Type == 0);
}

void test_message_cast(void** state) {
    TMessageHolder<TMessage> mes = NewHoldedMessage<TLogEntry>(static_cast<uint32_t>(TLogEntry::MessageType), 4);
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

    TTestTask h1 = [](TSocket& client, TMessageHolder<TLogEntry> mes) -> TTestTask
    {
        co_await client.Connect();
        auto r = co_await client.WriteSome(mes.RawData.get(), mes->Len);
        co_return;
    }(client, mes);

    TMessageHolder<TMessage> received;
    TTestTask h2 = [](TSocket& server, TMessageHolder<TMessage>& received) -> TTestTask
    {
        auto client = std::move(co_await server.Accept());
        uint32_t type, len;
        auto r = co_await client.ReadSome((char*)&type, sizeof(type));
        r = co_await client.ReadSome((char*)&len, sizeof(len));
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

void test_become(void**) {
    auto raft = MakeRaft();
    assert_true(raft->CurrentStateName() == EState::FOLLOWER);
    raft->Become(EState::CANDIDATE);
    assert_true(raft->CurrentStateName() == EState::CANDIDATE);
}

void test_become_same_func(void**) {
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft({}, 3, ts);
    assert_true(raft->CurrentStateName() == EState::FOLLOWER);
    ts->Advance(std::chrono::milliseconds(10000));
    raft->Become(EState::FOLLOWER);
    assert_true(raft->CurrentStateName() == EState::FOLLOWER);
}

void test_apply_empty_result(void**) {
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft({}, 3, ts);
    auto state = raft->GetState();
    auto volatileState = raft->GetVolatileState();
    assert_true(raft->CurrentStateName() == EState::FOLLOWER);
    raft->ApplyResult(ts->Now(), {});
    assert_true(raft->CurrentStateName() == EState::FOLLOWER);
    assert_true(state == raft->GetState());
    assert_true(volatileState == raft->GetVolatileState());
}

void test_apply_state_func_change_result(void**) {
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft({}, 3, ts);
    auto state = raft->GetState();
    auto volatileState = raft->GetVolatileState();
    assert_true(raft->CurrentStateName() == EState::FOLLOWER);
    raft->ApplyResult(ts->Now(), std::make_unique<TResult>(TResult {
        .NextStateName = EState::CANDIDATE
    }));
    assert_true(raft->CurrentStateName() == EState::CANDIDATE);
    assert_true(state == raft->GetState());
    assert_true(volatileState == raft->GetVolatileState());
}

void test_apply_time_change_result(void**) {
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft({}, 3, ts);
    auto n = ts->Now();
    raft->ApplyResult(n, std::make_unique<TResult>(TResult {
        .UpdateLastTime = true
    }));
    assert_true(raft->GetLastTime() == n);
}

void test_follower_to_candidate_on_timeout(void**) {
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft({}, 3, ts);
    assert_true(raft->CurrentStateName() == EState::FOLLOWER);
    raft->Process(NewTimeout());
    assert_true(raft->CurrentStateName() == EState::FOLLOWER);
    ts->Advance(std::chrono::milliseconds(10000));
    raft->Process(NewTimeout());
    assert_true(raft->CurrentStateName() == EState::CANDIDATE);
}

void test_follower_append_entries_small_term(void**) {
    std::vector<TMessageHolder<TMessage>> messages;
    auto onSend = [&](const TMessageHolder<TMessage>& message) {
        messages.push_back(message);
    };
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft(onSend, 3, ts);
    auto mes = NewHoldedMessage<TAppendEntriesRequest>();
    mes->Src = 2;
    mes->Dst = 1;
    mes->Term = 0;
    mes->LeaderId = 2;
    mes->PrevLogIndex = 0;
    mes->PrevLogTerm = 0;
    mes->LeaderCommit = 0;
    mes->Nentries = 0;
    raft->Process(mes);

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
    auto raft = MakeRaft(onSend, 3, ts);
    raft->SetState(TState{
        .CurrentTerm = 1,
        .VotedFor = 2,
        .Log = MakeLog<TLogEntry>({1,1,1,4,4,5,5,6,6})
    });
    auto mes = NewHoldedMessage<TAppendEntriesRequest>();
    mes->Src = 2;
    mes->Dst = 1;
    mes->Term = 1;
    mes->LeaderId = 2;
    mes->PrevLogIndex = 9;
    mes->PrevLogTerm = 6;
    mes->LeaderCommit = 9;
    mes->Nentries = 1;
    mes.Payload = MakeLog({6});
    raft->Process(mes);
    auto last = messages.back().Cast<TAppendEntriesResponse>();
    assert_true(last->Success);
    assert_true(last->MatchIndex = 10);
    assert_true(raft->GetState()->Log.size() == 10);
}

void test_follower_append_entries_7b(void**) {
    // leader: 1,1,1,4,4,5,5,6,6,6
    std::vector<TMessageHolder<TMessage>> messages;
    auto onSend = [&](const TMessageHolder<TMessage>& message) {
        messages.push_back(message);
    };
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft(onSend, 3, ts);
    raft->SetState(TState{
        .CurrentTerm = 1,
        .VotedFor = 2,
        .Log = MakeLog<TLogEntry>({1,1,1,4})
    });
    auto mes = NewHoldedMessage<TAppendEntriesRequest>();
    mes->Src = 2;
    mes->Dst = 1;
    mes->Term = 1;
    mes->LeaderId = 2;
    mes->PrevLogIndex = 4;
    mes->PrevLogTerm = 4;
    mes->LeaderCommit = 9;
    mes->Nentries = 6;
    mes.Payload = MakeLog({4,5,5,6,6,6});
    raft->Process(mes);
    auto last = messages.back().Cast<TAppendEntriesResponse>();
    assert_true(last->Success);
    assert_true(last->MatchIndex = 10);
    assert_true(raft->GetState()->Log.size() == 10);
}

void test_follower_append_entries_7c(void**) {
    // leader: 1,1,1,4,4,5,5,6,6,6
    std::vector<TMessageHolder<TMessage>> messages;
    auto onSend = [&](const TMessageHolder<TMessage>& message) {
        messages.push_back(message);
    };
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft(onSend, 3, ts);
    raft->SetState(TState{
        .CurrentTerm = 1,
        .VotedFor = 2,
        .Log = MakeLog<TLogEntry>({1,1,1,4,4,5,5,6,6,6,6})
    });
    auto mes = NewHoldedMessage<TAppendEntriesRequest>();
    mes->Src = 2;
    mes->Dst = 1;
    mes->Term = 1;
    mes->LeaderId = 2;
    mes->PrevLogIndex = 9;
    mes->PrevLogTerm = 6;
    mes->LeaderCommit = 9;
    mes->Nentries = 1;
    mes.Payload = MakeLog({6});
    raft->Process(mes);
    auto last = messages.back().Cast<TAppendEntriesResponse>();
    assert_true(last->Success);
    assert_true(last->MatchIndex = 10);
    assert_true(raft->GetState()->Log.size() == 11);
}

void test_follower_append_entries_7f(void**) {
    // leader: 1,1,1,4,4,5,5,6,6,6
    std::vector<TMessageHolder<TMessage>> messages;
    auto onSend = [&](const TMessageHolder<TMessage>& message) {
        messages.push_back(message);
    };
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft(onSend, 3, ts);
    raft->SetState(TState{
        .CurrentTerm = 1,
        .VotedFor = 2,
        .Log = MakeLog<TLogEntry>({1,1,1,2,2,2,3,3,3,3,3})
    });
    auto mes = NewHoldedMessage<TAppendEntriesRequest>();
    mes->Src = 2;
    mes->Dst = 1;
    mes->Term = 8;
    mes->LeaderId = 2;
    mes->PrevLogIndex = 3;
    mes->PrevLogTerm = 1;
    mes->LeaderCommit = 9;
    mes->Nentries = 7;
    mes.Payload = MakeLog({4,4,5,5,6,6,6});
    raft->Process(mes);
    auto last = messages.back().Cast<TAppendEntriesResponse>();
    assert_true(last->Success);
    assert_true(last->MatchIndex = 10);
    assert_true(raft->GetState()->Log.size() == 10);
    assert_terms(raft->GetState()->Log, {1,1,1,4,4,5,5,6,6,6});
}

void test_follower_append_entries_empty_to_empty_log(void**) {
    std::vector<TMessageHolder<TMessage>> messages;
    auto onSend = [&](const TMessageHolder<TMessage>& message) {
        messages.push_back(message);
    };
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft(onSend, 3, ts);
    auto mes = NewHoldedMessage<TAppendEntriesRequest>();
    mes->Src = 2;
    mes->Dst = 1;
    mes->Term = 1;
    mes->LeaderId = 2;
    mes->PrevLogIndex = 0;
    mes->PrevLogTerm = 0;
    mes->LeaderCommit = 0;
    mes->Nentries = 0;
    raft->Process(mes);
    assert_true(!messages.empty());
    auto last = messages.back().Cast<TAppendEntriesResponse>();
    assert_int_equal(last->Dst, 2);
    assert_true(last->Success);
    assert_int_equal(last->MatchIndex, 0);
}

void test_candidate_initiate_election(void**) {
    std::vector<TMessageHolder<TMessage>> messages;
    auto onSend = [&](const TMessageHolder<TMessage>& message) {
        messages.push_back(message);
    };
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft(onSend, 3, ts);
    ts->Advance(std::chrono::milliseconds(10000));
    auto term = raft->GetState()->CurrentTerm;
    raft->Become(EState::CANDIDATE);
    assert_int_equal(raft->GetState()->CurrentTerm, term+1);
    assert_true(ts->Now() == raft->GetLastTime());
    assert_int_equal(messages.size(), 2);
    TRequestVoteRequest r;
    r.Type = static_cast<uint32_t>(EMessageType::REQUEST_VOTE_REQUEST);
    r.Len = sizeof(r);
    r.Src = 1;
    r.Dst = 0;
    r.Term = term+1;
    r.CandidateId = raft->GetId();
    r.LastLogTerm = 0;
    r.LastLogIndex = 0;

    assert_message_equal(messages[0], r);
    assert_message_equal(messages[1], r);
}

void test_candidate_vote_request_small_term(void**) {
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft({}, 3, ts);
    auto req = NewHoldedMessage<TRequestVoteRequest>();
    req->Src = 2;
    req->Dst = 1;
    req->Term = 0;
    req->CandidateId = 2;
    req->LastLogTerm = 1;
    req->LastLogIndex = 1;
    auto result = raft->Candidate(ts->Now(), std::move(req));
    TRequestVoteResponse r;
    r.Type = static_cast<uint32_t>(EMessageType::REQUEST_VOTE_RESPONSE);
    r.Len = sizeof(r);
    r.Src = 1;
    r.Dst = 2;
    r.Term = raft->GetState()->CurrentTerm;
    r.VoteGranted = false;
    assert_message_equal(result->Message, r);
    assert_int_equal(raft->GetState()->CurrentTerm, 1);
}

void test_candidate_vote_request_ok_term(void**) {
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft({}, 3, ts);
    auto req = NewHoldedMessage<TRequestVoteRequest>();
    req->Src = 2;
    req->Dst = 1;
    req->Term = 1;
    req->CandidateId = 2;
    req->LastLogTerm = 1;
    req->LastLogIndex = 1;
    auto result = raft->Candidate(ts->Now(), std::move(req));
    TRequestVoteResponse r;
    r.Type = static_cast<uint32_t>(EMessageType::REQUEST_VOTE_RESPONSE);
    r.Len = sizeof(r);
    r.Src = 1;
    r.Dst = 2;
    r.Term = raft->GetState()->CurrentTerm;
    r.VoteGranted = true;
    assert_message_equal(result->Message, r);
    assert_int_equal(raft->GetState()->CurrentTerm, 1);
}

int main() {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_empty),
        cmocka_unit_test(test_message_create),
        cmocka_unit_test(test_message_cast),
        cmocka_unit_test(test_message_send_recv),
        cmocka_unit_test(test_initial),
        cmocka_unit_test(test_become),
        cmocka_unit_test(test_become_same_func),
        cmocka_unit_test(test_apply_empty_result),
        cmocka_unit_test(test_apply_state_func_change_result),
        cmocka_unit_test(test_apply_time_change_result),
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
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
