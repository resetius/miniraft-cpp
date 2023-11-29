#include <chrono>
#include <coroutine>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <vector>
#include "server.h"
#include "messages.h"

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

void TNode::Send(TMessageHolder<TMessage> message) {
    Messages.emplace_back(std::move(message));
}

void TNode::Drain() {
    if (!Connected) {
        Connect();
        return;
    }
    if (!Drainer || Drainer.done()) {
        if (Drainer && Drainer.done()) {
            Drainer.destroy();
        }
        Drainer = DoDrain();
    }
}

NNet::TTestTask TNode::DoDrain() {
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
    if (Address && (!Connector || Connector.done())) {
        if (Connector && Connector.done()) {
            Connector.destroy();
        }

        Socket = NNet::TSocket(*Address, Poller);
        Connected = false;
        Connector = DoConnect();
    }
}

NNet::TTestTask TNode::DoConnect() {
    std::cout << "Connecting " << Name << "\n";
    while (!Connected) {
        try {
            auto deadline = NNet::TClock::now() + std::chrono::milliseconds(100); // TODO: broken timeout in coroio
            co_await Socket.Connect(deadline);
            std::cout << "Connected " << Name << "\n";
            Connected = true;
        } catch (const std::exception& ex) {
            std::cout << "Error on connect: " << Name << " " << ex.what() << "\n";
        }
        if (!Connected) {
            co_await Poller.Sleep(std::chrono::milliseconds(1000));
        }
    }
    co_return;
}

NNet::TSimpleTask TRaftServer::InboundConnection(NNet::TSocket socket) {
    try {
        auto client = std::make_shared<TNode>(
            Poller, "client", std::move(socket), TimeSource
        );
        Nodes.insert(client);
        while (true) {
            auto mes = co_await TReader(client->Sock()).Read();
            std::cout << "Got message " << mes->Type << "\n";
            Raft->Process(std::move(mes), client);
            Raft->ProcessTimeout(TimeSource->Now());
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
    for (const auto& node : Nodes) {
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
        Raft->ProcessTimeout(TimeSource->Now());
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
