#include <miniraft/net/server.h>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
extern "C" {
#include <cmocka.h>
}

namespace {
struct TPoller : NNet::TPollerBase {
    void Tick() { ProcessTimers(); }
};
// Model the dependency's connect awaiter: a pending timer retains the coroutine
// handle, and destroying the awaiter does not unregister that timer. The timer
// is already due, so the schedule needs neither sockets nor wall-clock waits.
struct TSocket {
    TPoller* Owner = nullptr;
    bool FailWrite = false;
    auto Connect(NNet::TTime) {
        struct TAwaiter {
            TPoller* Owner;
            bool await_ready() { return false; }
            void await_suspend(std::coroutine_handle<> h) { Owner->AddTimer({}, h); }
            void await_resume() {}
        };
        return TAwaiter{Owner};
    }
    TPoller* Poller() { return Owner; }
    NNet::TValueTask<ssize_t> WriteSome(const void*, size_t length) {
        if (FailWrite) throw std::runtime_error("injected write failure");
        co_return length;
    }
};
void test_disconnect_during_pending_connect_does_not_destroy_timer_target(void**) {
    TPoller poller;
    TNode<TSocket> node([&](const auto&) { return TSocket{&poller}; }, "peer",
        NNet::TAddress("127.0.0.1", 1), std::make_shared<TTimeSource>());
    node.Drain(); assert_false(node.IsConnected());
    node.Disconnect(); // Must preserve the active reconnect until it completes.
    poller.Tick();
    assert_true(node.IsConnected());
    node.Disconnect(); assert_false(node.IsConnected());
}
void test_old_reader_failure_cannot_disconnect_writer_reconnect(void**) {
    TPoller poller;
    TNode<TSocket> node([&](const auto&) { return TSocket{&poller}; }, "peer",
        NNet::TAddress("127.0.0.1", 1), std::make_shared<TTimeSource>());
    node.Drain(); poller.Tick(); assert_true(node.IsConnected());
    const auto old = node.GetConnectionGeneration();
    node.Sock().FailWrite = true;
    node.Send(NewHoldedMessage<TCommandRequest>()); node.Drain();
    assert_false(node.IsConnected()); assert_true(node.GetConnectionGeneration() != old);
    node.Disconnect(old); // Old reader unwinds while the new connect is pending.
    poller.Tick(); assert_true(node.IsConnected());
    node.Disconnect(old); // It is stale even after the replacement connected.
    assert_true(node.IsConnected());
    node.Send(NewHoldedMessage<TCommandRequest>()); node.Drain();
    node.Disconnect(node.GetConnectionGeneration()); assert_false(node.IsConnected());
}
}
int main() {
    const CMUnitTest tests[] = {
        cmocka_unit_test(test_disconnect_during_pending_connect_does_not_destroy_timer_target),
        cmocka_unit_test(test_old_reader_failure_cannot_disconnect_writer_reconnect),
    };
    return cmocka_run_group_tests(tests, nullptr, nullptr);
}
