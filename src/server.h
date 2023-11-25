#pragma once

#include <memory>
#include <coroutine>

#include <all.hpp>
#include "messages.h"
#include "raft.h"
#include "socket.hpp"


template<typename T>
struct TPromise
{
    struct TTask : std::coroutine_handle<TPromise<T>>
    {
        using promise_type = TPromise<T>;

        bool await_ready() {
            return this->promise().Value != nullptr;
        }

        void await_suspend(std::coroutine_handle<> caller) {
            this->promise().Caller = caller;
        }

        T await_resume() {
            return *this->promise().Value;
        }
    };

    TTask get_return_object() { return { TTask::from_promise(*this) }; }
    std::suspend_never initial_suspend() { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }

    void return_value(const T& t) {
        Value = std::make_shared<T>(t);
        if (Caller) {
            Caller.resume();
        }
    }

    void unhandled_exception() { }

    std::shared_ptr<T> Value;
    std::coroutine_handle<> Caller;
};

template<>
struct TPromise<void>
{
    struct TTask : std::coroutine_handle<TPromise<void>>
    {
        using promise_type = TPromise<void>;

        bool await_ready() {
            return this->promise().Ready;
        }

        void await_suspend(std::coroutine_handle<> caller) {
            this->promise().Caller = caller;
        }

        void await_resume() { }
    };

    TTask get_return_object() { return { TTask::from_promise(*this) }; }
    std::suspend_never initial_suspend() { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }

    void return_void() {
        Ready = true;
        if (Caller) {
            Caller.resume();
        }
    }
    void unhandled_exception() { }

    bool Ready = false;
    std::coroutine_handle<> Caller;
};

class TReader {
public:
    TReader(NNet::TPoll::TSocket& socket)
        : Socket(socket)
    { }

    TPromise<TMessageHolder<TMessage>>::TTask Read();

private:
    NNet::TPoll::TSocket& Socket;
};

class TWriter {
public:
    TWriter(NNet::TPoll::TSocket& socket)
        : Socket(socket)
    { }

    TPromise<void>::TTask Write(TMessageHolder<TMessage> message);

private:
    NNet::TPoll::TSocket& Socket;
};

class TRaftServer {
public:
    TRaftServer(NNet::TPoll& poller, NNet::TAddress address, const std::shared_ptr<TRaft>& raft, const TNodeDict& nodes)
        : Poller(poller)
        , Socket(std::move(address), Poller)
        , Raft(raft)
        , Nodes(nodes)
    { }

    NNet::TSimpleTask Serve();

private:
    NNet::TSimpleTask InboundCounnection(NNet::TSocket socket);
    NNet::TTestTask Connector(std::shared_ptr<INode> node);

    NNet::TPoll& Poller;
    NNet::TPoll::TSocket Socket;
    std::shared_ptr<TRaft> Raft;
    TNodeDict Nodes;
};
