#pragma once

// Template definitions kept visible for alternative socket implementations.
//
// DoConnect and DoDrain are eager TVoidTask coroutines: nobody holds their
// handles and they free their own frames when they return, so a frame is never
// destroyed while the poller may still resume it. Each one holds the state and
// its connection, so neither goes away under it. Retiring a connection shuts
// its socket down: the suspended I/O fails, the coroutine returns, and the last
// holder closes the socket.
template<typename TSocket>
struct TNode<TSocket>::TState: std::enable_shared_from_this<TState> {
    static constexpr std::chrono::milliseconds RetryDelay{1000};

    TState(const std::string& name, std::optional<NNet::TAddress> address,
           const std::shared_ptr<ITimeSource>& ts, std::function<TSocket(const NNet::TAddress&)> factory)
        : Name(name)
        , Address(std::move(address))
        , TimeSource(ts)
        , SocketFactory(std::move(factory))
    { }

    void Connect() {
        if (Closed || !Address || Connection || Attempt) {
            return;
        }
        auto now = TimeSource->Now();
        if (now < NextAttempt) {
            return;
        }
        NextAttempt = now + RetryDelay;
        std::cout << "Connecting " << Name << "\n";
        try {
            Attempt = std::make_shared<TConnection>(SocketFactory(*Address));
        } catch (const std::exception& ex) {
            std::cout << "Error on connect: " << Name << " " << ex.what() << "\n";
            return;
        }
        DoConnect(this->shared_from_this(), Attempt);
    }

    void StartDrain() {
        if (Connection && !Connection->Draining && !Messages.empty()) {
            Connection->Draining = true;
            DoDrain(this->shared_from_this(), Connection);
        }
    }

    void Retire(std::shared_ptr<TConnection> connection) {
        if (!connection->Closed) {
            connection->Closed = true;
            Shutdown(connection->Socket);
        }
        if (Connection == connection) {
            Connection = nullptr;
        }
        if (Attempt == connection) {
            Attempt = nullptr;
        }
    }

    void Close() {
        Closed = true;
        Messages.clear();
        if (Connection) {
            Retire(Connection);
        }
        if (Attempt) {
            Retire(Attempt);
        }
    }

    // Wakes the I/O suspended on the socket. coroio's TSslSocket does not
    // expose its descriptor; its I/O ends when the peer closes or resets.
    static void Shutdown(TSocket& socket) {
        if constexpr (requires { socket.Shutdown(); }) {
            socket.Shutdown();
        } else if constexpr (requires { socket.Fd(); }) {
            if (socket.Fd() >= 0) {
#ifdef _WIN32
                ::shutdown(static_cast<SOCKET>(socket.Fd()), SD_BOTH);
#else
                ::shutdown(socket.Fd(), SHUT_RDWR);
#endif
            }
        }
    }

    static NNet::TVoidTask DoConnect(std::shared_ptr<TState> state, std::shared_ptr<TConnection> attempt) {
        try {
            // The dependency's finite deadline can leave I/O and timer wakeups
            // registered for the same coroutine after a timeout. Use its default
            // I/O-only connect until cancellation of both sources is supported.
            // TCP connect failure timing is therefore controlled by the OS.
            co_await attempt->Socket.Connect();
        } catch (const std::exception& ex) {
            std::cout << "Error on connect: " << state->Name << " " << ex.what() << "\n";
            attempt->Closed = true;
        }
        if (state->Attempt == attempt) {
            state->Attempt = nullptr;
            if (attempt->Closed) {
                // The peer is unreachable; Raft resends what it still needs.
                state->Messages.clear();
            }
        }
        if (attempt->Closed) {
            co_return;
        }
        std::cout << "Connected " << state->Name << "\n";
        state->Connection = attempt;
        state->StartDrain();
    }

    static NNet::TVoidTask DoDrain(std::shared_ptr<TState> state, std::shared_ptr<TConnection> connection) {
        while (!connection->Closed && !state->Messages.empty()) {
            auto message = std::move(state->Messages.front());
            state->Messages.pop_front();
            try {
                co_await TMessageWriter(connection->Socket).Write(std::move(message));
            } catch (const std::exception& ex) {
                std::cout << "Error on write: " << state->Name << " " << ex.what() << "\n";
                // The cut message is dropped with the queue; Raft resends what
                // it needs. A stale connection leaves the queue alone.
                if (state->Connection == connection) {
                    state->Messages.clear();
                }
                state->Retire(connection);
            }
        }
        connection->Draining = false;
    }

    std::string Name;
    std::optional<NNet::TAddress> Address;
    std::shared_ptr<ITimeSource> TimeSource;
    std::function<TSocket(const NNet::TAddress&)> SocketFactory;

    std::shared_ptr<TConnection> Connection;
    std::shared_ptr<TConnection> Attempt;
    ITimeSource::Time NextAttempt = {};
    std::deque<TMessageHolder<TMessage>> Messages;
    bool Closed = false;
};

template<typename TSocket>
TNode<TSocket>::TNode(const std::function<TSocket(const NNet::TAddress&)> factory, const std::string& name, NNet::TAddress address, const std::shared_ptr<ITimeSource>& ts)
    : State(std::make_shared<TState>(name, std::move(address), ts, factory))
{ }

template<typename TSocket>
TNode<TSocket>::TNode(const std::string& name, TSocket socket, const std::shared_ptr<ITimeSource>& ts)
    : State(std::make_shared<TState>(name, std::nullopt, ts, nullptr))
{
    State->Connection = std::make_shared<TConnection>(std::move(socket));
}

template<typename TSocket>
TNode<TSocket>::~TNode() {
    State->Close();
}

template<typename TSocket>
void TNode<TSocket>::Send(TMessageHolder<TMessage> message) {
    if (State->Closed) {
        return;
    }
    // Check here: a write error in DoDrain drops the whole queue.
    if (auto sendable = MakeSendable(std::move(message))) {
        State->Messages.emplace_back(std::move(sendable));
    }
}

template<typename TSocket>
void TNode<TSocket>::Drain() {
    if (State->Connection) {
        State->StartDrain();
    } else {
        State->Connect();
    }
}

template<typename TSocket>
bool TNode<TSocket>::IsConnected() const {
    return !!State->Connection;
}

template<typename TSocket>
std::shared_ptr<typename TNode<TSocket>::TConnection> TNode<TSocket>::GetConnection() const {
    return State->Connection;
}

template<typename TSocket>
void TNode<TSocket>::Disconnect(const std::shared_ptr<TConnection>& connection) {
    State->Messages.clear();
    if (connection) {
        State->Retire(connection);
    }
}

template<typename TSocket>
void TNode<TSocket>::Close() {
    State->Close();
}
