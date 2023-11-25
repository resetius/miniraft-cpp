#include <chrono>
#include <coroutine>
#include <cstdint>
#include <iostream>
#include <memory>
#include <functional>

#include <poll.hpp>
#include <messages.h>
#include <raft.h>
#include <server.h>
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

TMessageHolder<TLogEntry> MakeEntry(const char* text) {
    auto mes = NewHoldedMessage<TLogEntry>(
        static_cast<uint32_t>(TLogEntry::MessageType),
        sizeof(TLogEntry) + strlen(text) + 1
    );
    strcpy(mes.Mes->Data, text);
    return mes;
}

} // namespace

void test_read_write(void**) {
    auto mes = MakeEntry("MESSAGE");

    TLoop<TPoll> loop;
    TSocket socket(TAddress{"127.0.0.1", 8888}, loop.Poller());
    socket.Bind();
    socket.Listen();

    TSocket client(TAddress{"127.0.0.1", 8888}, loop.Poller());

    TTestTask h1 = [](TSocket& client, TMessageHolder<TLogEntry> mes) -> TTestTask
    {
        co_await client.Connect();
        co_await TWriter(client).Write(std::move(mes));
        co_return;
    }(client, mes);

    TMessageHolder<TMessage> received;
    TTestTask h2 = [](TSocket& server, TMessageHolder<TMessage>& received) -> TTestTask
    {
        auto client = std::move(co_await server.Accept());
        received = co_await TReader(client).Read();
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
}

void test_read_write_payload(void**) {
    auto mes = NewHoldedMessage<TAppendEntriesRequest>();
    char buf[1024];
    for (int i = 0; i < 1337; i++) {
        snprintf(buf, sizeof(buf), "message_%d_data", i);
        mes.Payload.push_back(MakeEntry(buf));
    }
    mes->Nentries = mes.Payload.size();

    TLoop<TPoll> loop;
    TSocket socket(TAddress{"127.0.0.1", 8889}, loop.Poller());
    socket.Bind();
    socket.Listen();

    TSocket client(TAddress{"127.0.0.1", 8889}, loop.Poller());

    TTestTask h1 = [](TSocket& client, TMessageHolder<TMessage> mes) -> TTestTask
    {
        co_await client.Connect();
        co_await TWriter(client).Write(std::move(mes));
        co_return;
    }(client, mes);

    TMessageHolder<TMessage> received;
    TTestTask h2 = [](TSocket& server, TMessageHolder<TMessage>& received) -> TTestTask
    {
        auto client = std::move(co_await server.Accept());
        received = co_await TReader(client).Read();
        co_return;
    }(socket, received);

    while (!(h1.done() && h2.done())) {
        loop.Step();
    }

    auto maybeCasted = received.Maybe<TAppendEntriesRequest>();
    assert_true(maybeCasted);
    auto casted = maybeCasted.Cast();
    assert_int_equal(mes->Nentries, casted->Nentries);

    h1.destroy(); h2.destroy();
}

int main() {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_read_write),
        cmocka_unit_test(test_read_write_payload),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
};