#pragma once

#include <exception>
#include <memory>
#include <coroutine>
#include <string_view>
#include <charconv>
#include <optional>

#include <coroio/all.hpp>

#include <miniraft/timesource.h>
#include <miniraft/messages.h>
#include <miniraft/raft.h>

template<typename TSocket>
class TMessageReader {
public:
    TMessageReader(TSocket& socket, TMessageLimits limits = {})
        : Socket(socket), Limits(limits) { ValidateMessageLimits(Limits); }

    NNet::TValueTask<TMessageHolder<TMessage>> Read() {
        if (Failed) throw TMessageError("Cannot reuse a failed frame reader");
        try {
            uint64_t remaining = Limits.MaxBatchBytes;
            co_return co_await ReadFrame(false, remaining);
        } catch (...) {
            Failed = true;
            throw;
        }
    }

private:
    NNet::TValueTask<TMessageHolder<TMessage>> ReadFrame(bool logOnly, uint64_t& remaining) {
        TMessage header;
        co_await NNet::TByteReader(Socket).Read(&header, sizeof(header));
        ValidateMessageHeader(header.Type, header.Len, Limits);
        if (logOnly && header.Type != uint32_t(EMessageType::LOG_ENTRY))
            throw TMessageError("AppendEntries children must be log entries");
        if (header.Len > remaining) throw TMessageError("Message batch exceeds byte limit");
        remaining -= header.Len;
        auto message = NewHoldedMessage<TMessage>(header.Type, header.Len);
        co_await NNet::TByteReader(Socket).Read(message->Value, header.Len - sizeof(TMessage));
        if (header.Type == uint32_t(EMessageType::APPEND_ENTRIES_REQUEST)) {
            const auto count = message.template Cast<TAppendEntriesRequest>()->Nentries;
            if (count > Limits.MaxEntries || uint64_t(count) * sizeof(TLogEntry) > remaining)
                throw TMessageError("AppendEntries count exceeds limits");
            message.InitPayload(count);
            for (uint32_t i = 0; i < count; ++i)
                message.Payload[i] = co_await ReadFrame(true, remaining);
        }
        co_return message;
    }
    TSocket& Socket;
    TMessageLimits Limits;
    bool Failed = false;
};

template<typename TSocket>
class TMessageWriter {
public:
    TMessageWriter(TSocket& socket, TMessageLimits limits = {})
        : Socket(socket), Limits(limits) { ValidateMessageLimits(Limits); }

    NNet::TValueTask<void> Write(TMessageHolder<TMessage> message) {
        ValidateMessageTree(message, Limits);
        co_await NNet::TByteWriter(Socket).Write(message.Mes, message->Len);
        for (uint32_t i = 0; i < message.PayloadSize; ++i)
            co_await Write(message.Payload[i]);
    }
private:
    TSocket& Socket;
    TMessageLimits Limits;
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
    // Called after a failed outbound read, once its reader has unwound.
    void Disconnect();
    bool IsConnected() { return Connected; }
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
        const std::shared_ptr<IRsm>& rsm,
        const TNodeDict& nodes,
        const std::shared_ptr<ITimeSource>& ts)
        : Poller(poller)
        , Socket(std::move(socket))
        , Raft(raft)
        , RequestProcessor(std::make_shared<TRequestProcessor>(raft, rsm, nodes))
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
    NNet::TVoidTask OutboundServe(std::shared_ptr<TNode<TSocket>>);
    NNet::TVoidTask Idle();
    void DrainNodes();
    void DebugPrint();

    typename TSocket::TPoller& Poller;
    TSocket Socket;
    std::shared_ptr<TRaft> Raft;
    std::shared_ptr<TRequestProcessor> RequestProcessor;
    std::unordered_set<std::shared_ptr<INode>> Nodes;
    std::shared_ptr<ITimeSource> TimeSource;

    // for debug
    TStateFields PersistentFields;
    TVolatileState VolatileFields;
};

