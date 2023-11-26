#include <chrono>
#include <coroutine>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <vector>
#include "server.h"
#include "messages.h"

namespace {
class TClientNode: public INode {
public:
    void Send(const TMessageHolder<TMessage>& mes) override {
        Messages.push_back(mes);
    }

    void Drain() override { }

    std::vector<TMessageHolder<TMessage>> Messages;
};

};

TPromise<void>::TTask TWriter::Write(TMessageHolder<TMessage> message) {
    auto payload = std::move(message.Payload);
    char* p = (char*)message.Mes; // TODO: const char
    uint32_t len = message->Len;
    while (len != 0) {
        auto written = co_await Socket.WriteSome(p, len);
        if (written == 0) {
            throw std::runtime_error("Connection closed");
        }
        if (written < 0) {
            continue; // retry;
        }
        p += written;
        len -= written;
    }

    for (auto&& m : payload) {
        co_await TWriter(Socket).Write(std::move(m));
    }

    co_return;
}

TPromise<TMessageHolder<TMessage>>::TTask TReader::Read() {
    decltype(TMessage::Type) type;
    decltype(TMessage::Len) len;
    auto s = co_await Socket.ReadSome((char*)&type, sizeof(type));
    if (s != sizeof(type)) {
        throw std::runtime_error("Connection closed");
    }
    s = co_await Socket.ReadSome((char*)&len, sizeof(len));
    if (s != sizeof(len)) {
        throw std::runtime_error("Connection closed");
    }
    auto mes = NewHoldedMessage<TMessage>(type, len);
    char* p = mes->Value;
    len -= sizeof(TMessage);
    while (len != 0) {
        s = co_await Socket.ReadSome(p, len);
        if (s == 0) {
            throw std::runtime_error("Connection closed");
        }
        if (s < 0) {
            continue; // retry
        }
        p += s;
        len -= s;
    }
    auto maybeAppendEntries = mes.Maybe<TAppendEntriesRequest>();
    if (maybeAppendEntries) {
        auto appendEntries = maybeAppendEntries.Cast();
        mes.Payload.resize(appendEntries->Nentries);
        for (auto& m : mes.Payload) {
            m = co_await TReader(Socket).Read();
        }
    }
    co_return mes;
}

void TNode::Send(const TMessageHolder<TMessage>& message) {
    Messages.emplace_back(message);
}

void TNode::Drain() {
    if (!Drainer || Drainer.done()) {
        if (Drainer && Drainer.done()) {
            Drainer.destroy();
        }
        Drainer = DoDrain();
    }
}

NNet::TTestTask TNode::DoDrain() {
    if (!Connected) {
        Connect();
        co_return;
    }
    auto tosend = std::move(Messages);
    try {
        for (auto&& m : tosend) {
            co_await TWriter(Socket).Write(std::move(m));
        }
    } catch (const std::exception& ex) {
        std::cout << "Error on write: " << ex.what() << "\n";
        Connect();
    }
    Messages.clear();
    co_return;
}

void TNode::Connect() {
    if (!Connector || Connector.done()) {
        if (Connector && Connector.done()) {
            Connector.destroy();
        }

        Socket = NNet::TSocket(Address, Poller);
        Connected = false;
        Connector = DoConnect();
    }
}

NNet::TTestTask TNode::DoConnect() {
    std::cout << "Connecting " << Id << "\n";
    while (!Connected) {
        try {
            auto deadline = NNet::TClock::now() + std::chrono::milliseconds(100); // TODO: broken timeout in coroio
            co_await Socket.Connect(deadline);
            std::cout << "Connected " << Id << "\n";
            Connected = true;
        } catch (const std::exception& ex) {
            std::cout << "Error on connect: " << Id << " " << ex.what() << "\n";
        }
        if (!Connected) {
            co_await Poller.Sleep(std::chrono::milliseconds(1000));
        }
    }
    co_return;
}

NNet::TSimpleTask TRaftServer::InboundConnection(NNet::TSocket socket) {
    try {
        TClientNode client;
        while (true) {
            auto mes = co_await TReader(socket).Read();
            std::cout << "Got message " << mes->Type << "\n";
            Raft->Process(std::move(mes), &client);
            if (!client.Messages.empty()) {
                auto tosend = std::move(client.Messages); client.Messages.clear();
                for (auto&& mes : tosend) {
                    co_await TWriter(socket).Write(std::move(mes));
                }
            }
            DrainNodes();
        }
    } catch (const std::exception & ex) {
        std::cerr << "Exception: " << ex.what() << "\n";
    }
    co_return;
}

void TRaftServer::Serve() {
    Idle();
    InboundServe();
}

void TRaftServer::DrainNodes() {
    for (auto [id, node] : Nodes) {
        node->Drain();
    }
}

NNet::TSimpleTask TRaftServer::InboundServe() {
    std::cout << "Bind\n";
    Socket.Bind();
    std::cout << "Listen\n";
    Socket.Listen();
    while (true) {
        auto client = co_await Socket.Accept();
        std::cout << "Accepted\n";
        InboundConnection(std::move(client));
    }
    co_return;
}

NNet::TSimpleTask TRaftServer::Idle() {
    auto t0 = TimeSource->Now();
    auto dt = std::chrono::milliseconds(2000);
    auto sleep = std::chrono::milliseconds(100);
    while (true) {
        Raft->Process(NewTimeout());
        DrainNodes();
        auto t1 = TimeSource->Now();
        if (t1 > t0 + dt) {
            std::cout << "Idle " << (uint32_t)Raft->CurrentStateName() << "\n";
            t0 = t1;
        }
        co_await Poller.Sleep(sleep);
    }
    co_return;
}