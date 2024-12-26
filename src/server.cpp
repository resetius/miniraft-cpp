#include <chrono>
#include <coroutine>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <vector>
#include "raft.h"
#include "server.h"
#include "messages.h"

template<typename TSocket>
NNet::TValueTask<void> TMessageWriter<TSocket>::Write(TMessageHolder<TMessage> message) {
    co_await NNet::TByteWriter(Socket).Write(message.Mes, message->Len);

    auto payload = std::move(message.Payload);
    for (uint32_t i = 0; i < message.PayloadSize; ++i) {
        co_await Write(std::move(payload[i]));
    }

    co_return;
}

template<typename TSocket>
NNet::TValueTask<TMessageHolder<TMessage>> TMessageReader<TSocket>::Read() {
    decltype(TMessage::Type) type;
    decltype(TMessage::Len) len;
    auto s = co_await Socket.ReadSome(&type, sizeof(type));
    if (s != sizeof(type)) {
        throw std::runtime_error("Connection closed");
    }
    s = co_await Socket.ReadSome(&len, sizeof(len));
    if (s != sizeof(len)) {
        throw std::runtime_error("Connection closed");
    }
    auto mes = NewHoldedMessage<TMessage>(type, len);
    co_await NNet::TByteReader(Socket).Read(mes->Value, len - sizeof(TMessage));
    auto maybeAppendEntries = mes.Maybe<TAppendEntriesRequest>();
    if (maybeAppendEntries) {
        auto appendEntries = maybeAppendEntries.Cast();
        auto nentries = appendEntries->Nentries;
        mes.InitPayload(nentries);
        for (uint32_t i = 0; i < nentries; i++) {
            mes.Payload[i] = co_await Read();
        }
    }
    co_return mes;
}

template<typename TSocket>
void TNode<TSocket>::Send(TMessageHolder<TMessage> message) {
    Messages.emplace_back(std::move(message));
}

template<typename TSocket>
void TNode<TSocket>::Drain() {
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

template<typename TSocket>
NNet::TVoidSuspendedTask TNode<TSocket>::DoDrain() {
    try {
        while (!Messages.empty()) {
            auto tosend = std::move(Messages); Messages.clear();
            for (auto&& m : tosend) {
                co_await TMessageWriter(Socket).Write(std::move(m));
            }
        }
    } catch (const std::exception& ex) {
        std::cout << "Error on write: " << ex.what() << "\n";
        Connect();
    }
    co_return;
}

template<typename TSocket>
void TNode<TSocket>::Connect() {
    if (Address && (!Connector || Connector.done())) {
        if (Connector && Connector.done()) {
            Connector.destroy();
        }
        Connector = DoConnect();
    }
}

template<typename TSocket>
NNet::TVoidSuspendedTask TNode<TSocket>::DoConnect() {
    std::cout << "Connecting " << Name << "\n";
    Connected = false;
    while (!Connected) {
        try {
            auto deadline = NNet::TClock::now() + std::chrono::milliseconds(100); // TODO: broken timeout in coroio
            Socket = SocketFactory(*Address);
            co_await Socket.Connect(deadline);
            std::cout << "Connected " << Name << "\n";
            Connected = true;
        } catch (const std::exception& ex) {
            std::cout << "Error on connect: " << Name << " " << ex.what() << "\n";
        }
        if (!Connected) {
            co_await Socket.Poller()->Sleep(std::chrono::milliseconds(1000));
        }
    }
    co_return;
}

template<typename TSocket>
NNet::TVoidTask TRaftServer<TSocket>::InboundConnection(TSocket socket) {
    std::shared_ptr<TNode<TSocket>> client;
    try {
        client = std::make_shared<TNode<TSocket>>(
            "client", std::move(socket), TimeSource
        );
        Nodes.insert(client);
        while (true) {
            auto mes = co_await TMessageReader(client->Sock()).Read();
            // client request 
            if (auto maybeCommandRequest = mes.template Maybe<TCommandRequest>()) {
                RequestProcessor->OnCommandRequest(std::move(maybeCommandRequest.Cast()), client);
            } else if (auto maybeCommandResponse = mes.template Maybe<TCommandResponse>()) {
                RequestProcessor->OnCommandResponse(std::move(maybeCommandResponse.Cast()));
            } else {
                Raft->Process(TimeSource->Now(), std::move(mes), client);
            }
            Raft->ProcessTimeout(TimeSource->Now());
            RequestProcessor->CheckStateChange();
            RequestProcessor->ProcessCommitted();
            RequestProcessor->ProcessWaiting();
            DrainNodes();
        }
    } catch (const std::exception & ex) {
        std::cerr << "Exception: " << ex.what() << "\n";
    }
    Nodes.erase(client);
    RequestProcessor->CleanUp(client);
    co_return;
}

template<typename TSocket>
void TRaftServer<TSocket>::Serve() {
    Idle();
    InboundServe();
    std::vector<NNet::TVoidTask> tasks;
    for (auto& node : Nodes) {
        auto realNode = std::dynamic_pointer_cast<TNode<TSocket>>(node);
        if (realNode) {
            tasks.emplace_back(OutboundServe(realNode));
        }
    }
}

template<typename TSocket>
void TRaftServer<TSocket>::DrainNodes() {
    for (const auto& node : Nodes) {
        node->Drain();
    }
}

template<typename TSocket>
NNet::TVoidTask TRaftServer<TSocket>::OutboundServe(std::shared_ptr<TNode<TSocket>> node) {
    // read forwarded replies
    while (true) {
        bool error = false;
        try {
            auto mes = co_await TMessageReader(node->Sock()).Read();
            if (auto maybeCommandResponse = mes.template Maybe<TCommandResponse>()) {
                RequestProcessor->OnCommandResponse(std::move(maybeCommandResponse.Cast()));
                RequestProcessor->ProcessWaiting();
                DrainNodes();
            } else {
                std::cerr << "Wrong message type: " << mes->Type << std::endl;
            }
        } catch (const std::exception& ex) {
            // wait for reconnection
            std::cerr << "Exception: " << ex.what() << std::endl;
            error = true;
        }
        if (error) {
            co_await Poller.Sleep(std::chrono::milliseconds(1000));
        }
    }
    co_return;
}

template<typename TSocket>
NNet::TVoidTask TRaftServer<TSocket>::InboundServe() {
    while (true) {
        auto client = co_await Socket.Accept();
        std::cout << "Accepted\n";
        InboundConnection(std::move(client));
    }
    co_return;
}

template<typename TSocket>
void TRaftServer<TSocket>::DebugPrint() {
    const TStateFields& state = *Raft->GetState();
    const TVolatileState& volatileState = *Raft->GetVolatileState();
    if (state == PersistentFields && volatileState == VolatileFields) {
        return;
    }
    if (Raft->CurrentStateName() == EState::LEADER) {
        std::cout << "Leader, "
            << "Term: " << state.CurrentTerm << ", "
            << "Index: " << state.LastLogIndex << ", "
            << "CommitIndex: " << volatileState.CommitIndex << ", ";
        std::cout << "Delay: ";
        for (auto [id, index] : volatileState.MatchIndex) {
            std::cout << id << ":" << (state.LastLogIndex - index) << " ";
        }
        std::cout << "MatchIndex: ";
        for (auto [id, index] : volatileState.MatchIndex) {
            std::cout << id << ":" << index << " ";
        }
        std::cout << "NextIndex: ";
        for (auto [id, index] : volatileState.NextIndex) {
            std::cout << id << ":" << index << " ";
        }
        std::cout << "\n";
    } else if (Raft->CurrentStateName() == EState::CANDIDATE) {
        std::cout << "Candidate, "
            << "Term: " << state.CurrentTerm << ", "
            << "Index: " << state.LastLogIndex << ", "
            << "CommitIndex: " << volatileState.CommitIndex << ", "
            << "\n";
    } else if (Raft->CurrentStateName() == EState::FOLLOWER) {
        std::cout << "Follower, "
            << "Term: " << state.CurrentTerm << ", "
            << "Index: " << state.LastLogIndex << ", "
            << "CommitIndex: " << volatileState.CommitIndex << ", "
            << "\n";
    }
    PersistentFields = state;
    VolatileFields = volatileState;
}

template<typename TSocket>
NNet::TVoidTask TRaftServer<TSocket>::Idle() {
    auto t0 = TimeSource->Now();
    auto dt = std::chrono::milliseconds(2000);
    auto sleep = std::chrono::milliseconds(100);
    while (true) {
        Raft->ProcessTimeout(TimeSource->Now());
        DrainNodes();
        auto t1 = TimeSource->Now();
        if (t1 > t0 + dt) {
            DebugPrint();
            t0 = t1;
        }
        co_await Poller.Sleep(t1 + sleep);
    }
    co_return;
}

// TODO: Use TPoller::TSocket
template class TRaftServer<NNet::TSocket>;
template class TRaftServer<NNet::TSslSocket<NNet::TSocket>>;
