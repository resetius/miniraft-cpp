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
    void TickOne() {
        assert_false(Timers_.empty());
        auto timer = Timers_.top(); Timers_.pop();
        LastFiredTimer_ = timer.Id;
        if (timer.Handle) timer.Handle.resume();
    }
};
// Model the dependency's connect awaiter: a pending timer retains the coroutine
// handle, and destroying the awaiter does not unregister that timer. The timer
// is already due, so the schedule needs neither sockets nor wall-clock waits.
struct TSocket {
    TPoller* Owner = nullptr;
    bool FailWrite = false;
    bool DuplicateDeadline = false;
    auto Connect(NNet::TTime deadline = NNet::TTime::max()) {
        struct TAwaiter {
            TPoller* Owner;
            bool DuplicateDeadline;
            bool await_ready() { return false; }
            void await_suspend(std::coroutine_handle<> h) {
                Owner->AddTimer({}, h); // Controllable stand-in for I/O readiness.
                if (DuplicateDeadline) Owner->AddTimer({}, h);
            }
            void await_resume() {}
        };
        return TAwaiter{Owner, DuplicateDeadline && deadline != NNet::TTime::max()};
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
void test_connect_cannot_leave_a_second_deadline_wakeup_after_completion(void**) {
    TPoller poller;
    // The dependency's finite-deadline connect registers the same coroutine for
    // both I/O and a timer. Model a second wakeup surviving the first, as seen
    // in CI when timeout/I/O completion races with the retry sleep.
    TNode<TSocket> node([&](const auto&) { return TSocket{&poller, false, true}; }, "peer",
        NNet::TAddress("127.0.0.1", 1), std::make_shared<TTimeSource>());
    node.Drain(); poller.TickOne(); assert_true(node.IsConnected());
    node.Disconnect();
    poller.Tick(); // A leftover deadline must not resume the destroyed connector.
    assert_false(node.IsConnected());
}
}
int main() {
    const CMUnitTest tests[] = {
        cmocka_unit_test(test_disconnect_during_pending_connect_does_not_destroy_timer_target),
        cmocka_unit_test(test_old_reader_failure_cannot_disconnect_writer_reconnect),
        cmocka_unit_test(test_connect_cannot_leave_a_second_deadline_wakeup_after_completion),
    };
    return cmocka_run_group_tests(tests, nullptr, nullptr);
}
