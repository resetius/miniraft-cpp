#pragma once

// Template definitions kept visible for alternative socket implementations.
template<typename TSocket>
void TNode<TSocket>::Send(TMessageHolder<TMessage> message) {
    // Check here: a write error in DoDrain would drop the rest of the batch.
    if (auto sendable = MakeSendable(std::move(message))) {
        Messages.emplace_back(std::move(sendable));
    }
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
void TNode<TSocket>::Disconnect(uint64_t generation) {
    if (generation != ConnectionGeneration || (Connector && !Connector.done())) return;
    // DoConnect may already be retrying after a write failure. Its awaiter can
    // still be registered with the poller; never destroy that live coroutine
    // because an older reader failed. Established connectors are done here.
    Connected = false;
    if (Drainer) { Drainer.destroy(); Drainer = {}; }
    if (Connector) { Connector.destroy(); Connector = {}; }
    std::destroy_at(&Socket);
    std::construct_at(&Socket);
}

template<typename TSocket>
void TNode<TSocket>::Connect() {
    if (Address && (!Connector || Connector.done())) {
        if (ConnectionGeneration == UINT64_MAX) throw std::overflow_error("Connection generation exhausted");
        if (Connector && Connector.done()) {
            Connector.destroy();
        }
        ++ConnectionGeneration;
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

