#include <chrono>
#include <coroutine>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <vector>
#include "server.h"
#include "messages.h"

template<typename TSocket>
TPromise<void>::TTask TWriter<TSocket>::Write(TMessageHolder<TMessage> message) {
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

template<typename TSocket>
TPromise<TMessageHolder<TMessage>>::TTask TReader<TSocket>::Read() {
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

template<typename TPoller>
void TNode<TPoller>::Send(TMessageHolder<TMessage> message) {
    Messages.emplace_back(std::move(message));
}

template<typename TPoller>
void TNode<TPoller>::Drain() {
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

template<typename TPoller>
NNet::TTestTask TNode<TPoller>::DoDrain() {
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

template<typename TPoller>
void TNode<TPoller>::Connect() {
    if (Address && (!Connector || Connector.done())) {
        if (Connector && Connector.done()) {
            Connector.destroy();
        }

        Socket = NNet::TSocket(*Address, Poller);
        Connected = false;
        Connector = DoConnect();
    }
}

template<typename TPoller>
NNet::TTestTask TNode<TPoller>::DoConnect() {
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

template<typename TPoller>
NNet::TSimpleTask TRaftServer<TPoller>::InboundConnection(NNet::TSocket socket) {
    try {
        auto client = std::make_shared<TNode<TPoller>>(
            Poller, "client", std::move(socket), TimeSource
        );
        Nodes.insert(client);
        while (true) {
            auto mes = co_await TReader(client->Sock()).Read();
//            std::cout << "Got message " << mes->Type << "\n";
            Raft->Process(TimeSource->Now(), std::move(mes), client);
            Raft->ProcessTimeout(TimeSource->Now());
            DrainNodes();
        }
    } catch (const std::exception & ex) {
        std::cerr << "Exception: " << ex.what() << "\n";
    }
    co_return;
}

template<typename TPoller>
void TRaftServer<TPoller>::Serve() {
    Idle();
    InboundServe();
}

template<typename TPoller>
void TRaftServer<TPoller>::DrainNodes() {
    for (const auto& node : Nodes) {
        node->Drain();
    }
}

template<typename TPoller>
NNet::TSimpleTask TRaftServer<TPoller>::InboundServe() {
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

template<typename TPoller>
NNet::TSimpleTask TRaftServer<TPoller>::Idle() {
    auto t0 = TimeSource->Now();
    auto dt = std::chrono::milliseconds(2000);
    auto sleep = std::chrono::milliseconds(100);
    while (true) {
        Raft->ProcessTimeout(TimeSource->Now());
        DrainNodes();
        auto t1 = TimeSource->Now();
        if (t1 > t0 + dt) {
            std::cout << "Idle " << (uint32_t)Raft->CurrentStateName() << " " << Raft->GetState()->Log.size() << "\n";
            t0 = t1;
        }
        co_await Poller.Sleep(sleep);
    }
    co_return;
}

template class TRaftServer<NNet::TPoll>;
#ifdef __linux__
template class TRaftServer<NNet::TEPoll>;
#endif
