#include <chrono>
#include <coroutine>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <miniraft/raft.h>
#include <miniraft/messages.h>

#include "server.h"

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
                RequestProcessor->OnCommandResponse(std::move(maybeCommandResponse.Cast()), client);
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
        const bool wasConnected = node->IsConnected();
        const auto generation = node->GetConnectionGeneration();
        try {
            if (!node->IsConnected()) {
                throw std::runtime_error("Not connected");
            }
            auto mes = co_await TMessageReader(node->Sock()).Read();
            if (auto maybeCommandResponse = mes.template Maybe<TCommandResponse>()) {
                RequestProcessor->OnCommandResponse(std::move(maybeCommandResponse.Cast()), node);
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
            RequestProcessor->OnPeerDisconnected(node);
            if (wasConnected) node->Disconnect(generation);
            DrainNodes();
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
        RequestProcessor->CheckStateChange();
        RequestProcessor->ProcessCommitted();
        RequestProcessor->ProcessWaiting();
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
