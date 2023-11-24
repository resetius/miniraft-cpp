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
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
