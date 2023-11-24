#include <chrono>
#include <coroutine>
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
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
