#pragma once

#include <exception>
#include <memory>
#include <coroutine>
#include <string_view>
#include <charconv>
#include <optional>

#include <coroio/all.hpp>

#include "timesource.h"
#include "messages.h"
#include "raft.h"


template<typename T>
struct TPromise
{
    struct TTask : std::coroutine_handle<TPromise<T>>
    {
        using promise_type = TPromise<T>;

        ~TTask() { this->destroy(); /*TODO:*/ }

        bool await_ready() {
            return this->promise().Value != nullptr;
        }

        void await_suspend(std::coroutine_handle<> caller) {
            this->promise().Caller = caller;
        }

        T await_resume() {
            if (this->promise().Exception) {
                std::rethrow_exception(this->promise().Exception);
            } else {
                return *this->promise().Value;
            }
        }
    };

    TTask get_return_object() { return { TTask::from_promise(*this) }; }
    std::suspend_never initial_suspend() { return {}; }
    auto final_suspend() noexcept { 
        struct TAwaitable {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<TPromise<T>> h) noexcept {
                return h.promise().Caller;
            }
            void await_resume() noexcept { }
        };
        return TAwaitable {};
    }

    void return_value(const T& t) {
        Value = std::make_shared<T>(t);
    }

    void unhandled_exception() {
        Exception = std::current_exception();
    }

    std::shared_ptr<T> Value;
    std::exception_ptr Exception;
    std::coroutine_handle<> Caller = std::noop_coroutine();
};

template<>
struct TPromise<void>
{
    struct TTask  : std::coroutine_handle<TPromise<void>>
    {
        using promise_type = TPromise<void>;

        ~TTask() { destroy(); /* TODO: */ }

        bool await_ready() {
            return this->promise().Ready;
        }

        void await_suspend(std::coroutine_handle<> caller) {
            this->promise().Caller = caller;
        }

        void await_resume() {
            if (this->promise().Exception) {
                std::rethrow_exception(this->promise().Exception);
            }
        }
    };

    TTask get_return_object() { return { TTask::from_promise(*this) }; }
    std::suspend_never initial_suspend() { return {}; }
    auto final_suspend() noexcept { 
        struct TAwaitable {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<TPromise<void>> h) noexcept {
                return h.promise().Caller;
            }
            void await_resume() noexcept { }
        };
        return TAwaitable {};
    }


    void return_void() {
        Ready = true;
    }
    void unhandled_exception() {
        Exception = std::current_exception();
        Ready = true;
    }

    bool Ready = false;
    std::exception_ptr Exception;
    std::coroutine_handle<> Caller = std::noop_coroutine();
};

template<typename TSocket>
class TReader {
public:
    TReader(TSocket& socket)
        : Socket(socket)
    { }

    TPromise<TMessageHolder<TMessage>>::TTask Read();

private:
    TSocket& Socket;
};

template<typename TSocket>
class TWriter {
public:
    TWriter(TSocket& socket)
        : Socket(socket)
    { }

    TPromise<void>::TTask Write(TMessageHolder<TMessage> message);

private:
    TSocket& Socket;
};

struct THost {
    std::string Address;
    int Port = 0;
    uint32_t Id = 0;

    THost() { }

    THost(const std::string& str) {
        std::string_view s(str);
        auto p = s.find(':');
        Address = s.substr(0, p);
        s = s.substr(p + 1);
        p = s.find(':');
        std::from_chars(s.begin(), s.begin()+p, Port);
        s = s.substr(p + 1);
        std::from_chars(s.begin(), s.begin()+p, Id);
    }

    void DebugPrint() const {
        std::cout << "Addr: '" << Address << "'\n";
        std::cout << "Port: " << Port << "\n";
        std::cout << "Id: " << Id << "\n";
    }
};

template<typename TPoller>
class TNode: public INode {
public:
    TNode(TPoller& poller, const std::string& name, NNet::TAddress address, const std::shared_ptr<ITimeSource>& ts)
        : Poller(poller)
        , Name(name)
        , Address(address)
        , TimeSource(ts)
    { }

    TNode(TPoller& poller, const std::string& name, typename TPoller::TSocket socket, const std::shared_ptr<ITimeSource>& ts)
        : Poller(poller)
        , Name(name)
        , Socket(std::move(socket))
        , Connected(true)
        , TimeSource(ts)
    { }

    void Send(TMessageHolder<TMessage> message) override;
    void Drain() override;
    typename TPoller::TSocket& Sock() {
        return Socket;
    }

private:
    void Connect();

    NNet::TTestTask DoDrain();
    NNet::TTestTask DoConnect();

    TPoller& Poller;
    std::string Name;
    std::optional<NNet::TAddress> Address;
    std::shared_ptr<ITimeSource> TimeSource;
    typename TPoller::TSocket Socket;
    bool Connected = false;

    std::coroutine_handle<> Drainer;
    std::coroutine_handle<> Connector;

    std::vector<TMessageHolder<TMessage>> Messages;
};

template<typename TPoller>
class TRaftServer {
public:
    TRaftServer(
        TPoller& poller,
        NNet::TAddress address,
        const std::shared_ptr<TRaft>& raft,
        const TNodeDict& nodes,
        const std::shared_ptr<ITimeSource>& ts)
        : Poller(poller)
        , Socket(std::move(address), Poller)
        , Raft(raft)
        , TimeSource(ts)
    {
        for (const auto& [_, node] : nodes) {
            Nodes.insert(node);
        }
    }

    void Serve();

private:
    NNet::TSimpleTask InboundServe();
    NNet::TSimpleTask InboundConnection(typename TPoller::TSocket socket);
    NNet::TSimpleTask Idle();
    void DrainNodes();

    TPoller& Poller;
    typename TPoller::TSocket Socket;
    std::shared_ptr<TRaft> Raft;
    std::unordered_set<std::shared_ptr<INode>> Nodes;
    std::shared_ptr<ITimeSource> TimeSource;
};
