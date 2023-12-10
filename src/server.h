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

template<typename TSocket>
class TMessageReader {
public:
    TMessageReader(TSocket& socket)
        : Socket(socket)
    { }

    NNet::TValueTask<TMessageHolder<TMessage>> Read();

private:
    TSocket& Socket;
};

template<typename TSocket>
class TMessageWriter {
public:
    TMessageWriter(TSocket& socket)
        : Socket(socket)
    { }

    NNet::TValueTask<void> Write(TMessageHolder<TMessage> message);

private:
    TSocket& Socket;
};

struct THost {
    std::string Address;
    int Port = 0;
    uint32_t Id = 0;

    THost() { }

    operator bool() const {
        return !!Id;
    }

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

    NNet::TVoidSuspendedTask DoDrain();
    NNet::TVoidSuspendedTask DoConnect();

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
    NNet::TVoidTask InboundServe();
    NNet::TVoidTask InboundConnection(typename TPoller::TSocket socket);
    NNet::TVoidTask Idle();
    void DrainNodes();
    void DebugPrint();

    TPoller& Poller;
    typename TPoller::TSocket Socket;
    std::shared_ptr<TRaft> Raft;
    std::unordered_set<std::shared_ptr<INode>> Nodes;
    std::shared_ptr<ITimeSource> TimeSource;
};
