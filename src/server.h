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

template<typename TSocket>
class TNode: public INode {
public:
    TNode(const std::function<TSocket(const NNet::TAddress&)> factory, const std::string& name, NNet::TAddress address, const std::shared_ptr<ITimeSource>& ts)
        : Name(name)
        , Address(address)
        , TimeSource(ts)
        , SocketFactory(factory)
    { }

    TNode(const std::string& name, TSocket socket, const std::shared_ptr<ITimeSource>& ts)
        : Name(name)
        , Socket(std::move(socket))
        , Connected(true)
        , TimeSource(ts)
    { }

    void Send(TMessageHolder<TMessage> message) override;
    void Drain() override;
    TSocket& Sock() {
        return Socket;
    }

private:
    void Connect();

    NNet::TVoidSuspendedTask DoDrain();
    NNet::TVoidSuspendedTask DoConnect();

    std::string Name;
    std::optional<NNet::TAddress> Address;
    std::shared_ptr<ITimeSource> TimeSource;
    std::function<TSocket(const NNet::TAddress&)> SocketFactory;
    TSocket Socket;
    bool Connected = false;

    std::coroutine_handle<> Drainer;
    std::coroutine_handle<> Connector;

    std::vector<TMessageHolder<TMessage>> Messages;
};

template<typename TSocket>
class TRaftServer {
public:
    TRaftServer(
        typename TSocket::TPoller& poller,
        TSocket socket,
        const std::shared_ptr<TRaft>& raft,
        const TNodeDict& nodes,
        const std::shared_ptr<ITimeSource>& ts)
        : Poller(poller)
        , Socket(std::move(socket))
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
    NNet::TVoidTask InboundConnection(TSocket socket);
    NNet::TVoidTask Idle();
    void DrainNodes();
    void DebugPrint();

    typename TSocket::TPoller& Poller;
    TSocket Socket;
    std::shared_ptr<TRaft> Raft;
    std::unordered_set<std::shared_ptr<INode>> Nodes;
    std::shared_ptr<ITimeSource> TimeSource;
};
