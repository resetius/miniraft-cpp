#include <miniraft/net/server.h>
#include <algorithm>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
extern "C" {
#include <cmocka.h>
}

namespace {

// Resumes the woken coroutines in batches, as one coroio poll does.
struct TFakeLoop {
    std::vector<std::coroutine_handle<>> Ready;

    void Step() {
        auto ready = std::exchange(Ready, {});
        for (auto h : ready) {
            h.resume();
        }
    }
};

// One fake connection. A write waits while Room is zero. Break() fails the
// I/O and wakes the waiting coroutines, the reader first, like a reset.
struct TFakeWire {
    TFakeLoop* Loop = nullptr;
    size_t Room = SIZE_MAX;
    std::string Written;
    bool Broken = false;
    bool ShutDown = false;
    NNet::TTime Deadline = {};
    std::coroutine_handle<> Connector;
    std::coroutine_handle<> Reader;
    std::coroutine_handle<> Writer;

    void Wake(std::coroutine_handle<>& h) {
        if (h) {
            Loop->Ready.push_back(std::exchange(h, {}));
        }
    }

    void Break() {
        Broken = true;
        Wake(Reader);
        Wake(Writer);
        Wake(Connector);
    }
};

struct TFakeSocket {
    std::shared_ptr<TFakeWire> Wire;

    auto Connect(NNet::TTime deadline = NNet::TTime::max()) {
        Wire->Deadline = deadline;
        struct TAwaitable {
            TFakeWire* Wire;
            bool await_ready() { return false; }
            void await_suspend(std::coroutine_handle<> h) { Wire->Connector = h; }
            void await_resume() {
                if (Wire->Broken) {
                    throw std::runtime_error("connection refused");
                }
            }
        };
        return TAwaitable{Wire.get()};
    }

    // Only a broken wire wakes a reader: it sees the end of the stream.
    auto ReadSome(void*, size_t) {
        struct TAwaitable {
            TFakeWire* Wire;
            bool await_ready() { return Wire->Broken; }
            void await_suspend(std::coroutine_handle<> h) { Wire->Reader = h; }
            ssize_t await_resume() { return 0; }
        };
        return TAwaitable{Wire.get()};
    }

    auto WriteSome(const void* data, size_t size) {
        struct TAwaitable {
            TFakeWire* Wire;
            const char* Data;
            size_t Size;
            bool await_ready() { return Wire->Broken || Wire->Room > 0; }
            void await_suspend(std::coroutine_handle<> h) { Wire->Writer = h; }
            ssize_t await_resume() {
                if (Wire->Broken) {
                    throw std::runtime_error("broken pipe");
                }
                auto n = std::min(Size, Wire->Room);
                Wire->Room -= n;
                Wire->Written.append(Data, n);
                return n;
            }
        };
        return TAwaitable{Wire.get(), static_cast<const char*>(data), size};
    }

    void Shutdown() {
        Wire->ShutDown = true;
        Wire->Break();
    }
};

using TFakeNode = TNode<TFakeSocket>;

struct TManualTime: ITimeSource {
    Time Current = {};
    Time Now() override { return Current; }
};

struct TFakeNet {
    TFakeLoop Loop;
    std::vector<std::shared_ptr<TFakeWire>> Wires; // one per connect attempt
    std::shared_ptr<TManualTime> Time = std::make_shared<TManualTime>();

    std::shared_ptr<TFakeNode> MakeNode() {
        return std::make_shared<TFakeNode>([this](const NNet::TAddress&) {
            auto wire = std::make_shared<TFakeWire>();
            wire->Loop = &Loop;
            Wires.push_back(wire);
            return TFakeSocket{wire};
        }, "peer", NNet::TAddress("127.0.0.1", 1), Time);
    }

    void Connect(TFakeNode& node) {
        node.Drain();
        Wires.back()->Wake(Wires.back()->Connector);
        Loop.Step();
        assert_true(node.IsConnected());
    }
};

TMessageHolder<TMessage> Entry(const std::string& text) {
    auto entry = NewHoldedMessage<TLogEntry>(sizeof(TLogEntry) + text.size());
    memcpy(entry->Data, text.data(), text.size());
    return entry;
}

std::string Bytes(const TMessageHolder<TMessage>& message) {
    return std::string(reinterpret_cast<const char*>(message.Mes), message->Len);
}

// Reads like TRaftServer::OutboundServe and reports the failure the same way.
NNet::TVoidTask ReadUntilFailure(std::shared_ptr<TFakeNode> node,
    std::shared_ptr<TFakeNode::TConnection> connection, bool* failed)
{
    try {
        co_await TMessageReader(connection->Socket).Read();
    } catch (const std::exception&) {
        node->Disconnect(connection);
        *failed = true;
    }
}

void test_write_failure_drops_queue_and_reconnects(void**) {
    TFakeNet net;
    auto node = net.MakeNode();
    net.Connect(*node);
    auto first = net.Wires.back();
    auto one = Entry("one"), two = Entry("two");
    first->Room = 10;
    node->Send(one);
    node->Send(two);
    node->Drain(); // suspended inside "one"
    assert_int_equal(first->Written.size(), 10);

    first->Break();
    net.Loop.Step();
    assert_false(node->IsConnected());
    assert_true(first->ShutDown);

    node->Drain(); // the next attempt waits for the retry delay
    assert_int_equal(net.Wires.size(), 1);
    net.Time->Current += std::chrono::seconds(1);
    net.Connect(*node);
    assert_int_equal(net.Wires.size(), 2);
    assert_true(net.Wires.back()->Written.empty());
    auto three = Entry("three");
    node->Send(three);
    node->Drain();
    assert_true(net.Wires.back()->Written == Bytes(three));
}

// coroio reports a refused connect as a success; the first write then fails.
void test_unreachable_peer_does_not_keep_messages(void**) {
    TFakeNet net;
    auto node = net.MakeNode();
    net.Connect(*node);
    net.Wires.back()->Break();
    for (int i = 0; i < 100; ++i) {
        node->Send(Entry("stale"));
    }
    node->Drain(); // the failed write takes the queue with it
    assert_false(node->IsConnected());

    for (int i = 0; i < 100; ++i) {
        node->Send(Entry("stale"));
    }
    net.Time->Current += std::chrono::seconds(1);
    node->Drain();
    net.Wires.back()->Break(); // so does a failed attempt
    net.Loop.Step();
    assert_false(node->IsConnected());

    // OutboundServe calls it right after failing the forwarded requests.
    node->Send(Entry("stale"));
    node->Disconnect(nullptr);

    auto fresh = Entry("fresh");
    node->Send(fresh);
    net.Time->Current += std::chrono::seconds(1);
    net.Connect(*node);
    assert_true(net.Wires.back()->Written == Bytes(fresh));
}

void test_read_failure_and_write_wakeup_in_one_batch(void**) {
    TFakeNet net;
    auto node = net.MakeNode();
    net.Connect(*node);
    auto wire = net.Wires.back();
    std::weak_ptr<TFakeNode::TConnection> connection = node->GetConnection();
    bool failed = false;
    ReadUntilFailure(node, node->GetConnection(), &failed);
    auto one = Entry("one");
    wire->Room = 10;
    node->Send(one);
    node->Drain();

    // The reader disconnects first; the drainer must still be alive after it.
    wire->Break();
    net.Loop.Step();
    assert_true(failed);
    assert_false(node->IsConnected());
    assert_true(connection.expired());

    net.Time->Current += std::chrono::seconds(1);
    net.Connect(*node);
    auto two = Entry("two");
    node->Send(two);
    node->Drain();
    assert_true(net.Wires.back()->Written == Bytes(two));
}

void test_stale_reader_cannot_disconnect_new_connection(void**) {
    TFakeNet net;
    auto node = net.MakeNode();
    net.Connect(*node);
    auto old = node->GetConnection();
    node->Disconnect(old);
    assert_false(node->IsConnected());

    net.Time->Current += std::chrono::seconds(1);
    node->Drain();
    node->Disconnect(old); // the pending connect survives
    auto wire = net.Wires.back();
    wire->Wake(wire->Connector);
    net.Loop.Step();
    assert_true(node->IsConnected());

    auto current = node->GetConnection();
    node->Disconnect(old);
    assert_true(node->GetConnection() == current);
    assert_false(wire->ShutDown);
    node->Disconnect(current);
    assert_false(node->IsConnected());
    assert_true(wire->ShutDown);
}

void test_destroy_node_with_suspended_drainer(void**) {
    TFakeNet net;
    auto node = net.MakeNode();
    net.Connect(*node);
    auto wire = net.Wires.back();
    wire->Room = 10;
    node->Send(Entry("one"));
    node->Drain();
    std::weak_ptr<TFakeNode::TConnection> connection = node->GetConnection();

    node.reset();
    assert_true(wire->ShutDown);
    assert_false(connection.expired()); // held by the suspended drainer
    net.Loop.Step();
    assert_true(connection.expired());
}

void test_destroy_node_with_pending_connect(void**) {
    TFakeNet net;
    auto node = net.MakeNode();
    node->Drain();
    auto wire = net.Wires.back();
    assert_true(wire->Deadline == NNet::TTime::max());

    node.reset();
    assert_true(wire->ShutDown);
    net.Loop.Step();
    // Only net.Wires and this test hold the wire once the connector returned.
    assert_int_equal(wire.use_count(), 2);
}

void test_unsendable_reply_is_rejected_alone(void**) {
    TFakeNet net;
    auto node = net.MakeNode();
    net.Connect(*node);
    auto big = NewHoldedMessage<TCommandResponse>(sizeof(TCommandResponse) + 16 * 1024 * 1024);
    big->Cookie = 7;
    auto one = Entry("one"), two = Entry("two");
    node->Send(one);
    node->Send(big);
    node->Send(two);
    node->Drain();
    auto rejected = NewHoldedMessage(TCommandResponse{.Cookie = 7, .ErrorCode = 2});
    assert_true(net.Wires.back()->Written == Bytes(one) + Bytes(rejected) + Bytes(two));
}

NNet::TVoidSuspendedTask ConnectSocket(NNet::TSocket& socket) {
    co_await socket.Connect();
}

NNet::TVoidSuspendedTask AcceptSocket(NNet::TSocket& listener, NNet::TSocket& accepted) {
    accepted = std::move(co_await listener.Accept());
}

// The peer never reads, so the drainer waits on a full socket; destroying the
// node must shut the socket down so the poller wakes the drainer.
void test_destroy_node_wakes_writer_on_full_socket(void**) {
    NNet::TLoop<NNet::TPoll> loop;
    NNet::TSocket listener(NNet::TAddress{"127.0.0.1", 8891}, loop.Poller());
    listener.Bind();
    listener.Listen();
    NNet::TSocket client(NNet::TAddress{"127.0.0.1", 8891}, loop.Poller());
    NNet::TSocket peer;
    auto connect = ConnectSocket(client);
    auto accept = AcceptSocket(listener, peer);
    while (!(connect.done() && accept.done())) {
        loop.Step();
    }
    connect.destroy();
    accept.destroy();

    auto node = std::make_shared<TNode<NNet::TSocket>>("peer", std::move(client), std::make_shared<TTimeSource>());
    std::weak_ptr<TNode<NNet::TSocket>::TConnection> connection = node->GetConnection();
    // More than both socket buffers can take, still within one frame.
    std::string text(15 * 1024 * 1024, 'x');
    node->Send(Entry(text));
    node->Drain();
    for (int i = 0; i < 10; ++i) {
        loop.Step();
    }

    node.reset();
    assert_false(connection.expired());
    for (int i = 0; i < 100 && !connection.expired(); ++i) {
        loop.Step();
    }
    assert_true(connection.expired());
}

} // namespace

int main() {
    NNet::TInitializer init; // ignores SIGPIPE: the woken write fails with EPIPE
    const CMUnitTest tests[] = {
        cmocka_unit_test(test_write_failure_drops_queue_and_reconnects),
        cmocka_unit_test(test_unreachable_peer_does_not_keep_messages),
        cmocka_unit_test(test_read_failure_and_write_wakeup_in_one_batch),
        cmocka_unit_test(test_stale_reader_cannot_disconnect_new_connection),
        cmocka_unit_test(test_destroy_node_with_suspended_drainer),
        cmocka_unit_test(test_destroy_node_with_pending_connect),
        cmocka_unit_test(test_unsendable_reply_is_rejected_alone),
        cmocka_unit_test(test_destroy_node_wakes_writer_on_full_socket),
    };
    return cmocka_run_group_tests(tests, nullptr, nullptr);
}
