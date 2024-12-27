#include <coroio/all.hpp>
#include <csignal>
#include <miniraft/timesource.h>
#include <miniraft/raft.h>
#include <miniraft/net/server.h>

void usage(const char* prog) {
    std::cerr << prog << " --id myid --node ip:port:id [--node ip:port:id ...]" << "\n";
    exit(0);
}

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    std::vector<THost> hosts;
    THost myHost;
    TNodeDict nodes;
    uint32_t id = 0;
    bool ssl = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--node") && i < argc - 1) {
            // address:port:id
            hosts.push_back(THost{argv[++i]});
        } else if (!strcmp(argv[i], "--id") && i < argc - 1) {
            id = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--ssl")) {
            ssl = true;
        } else if (!strcmp(argv[i], "--help")) {
            usage(argv[0]);
        }
    }

    using TPoller = NNet::TDefaultPoller;

    std::shared_ptr<ITimeSource> timeSource = std::make_shared<TTimeSource>();
    NNet::TLoop<TPoller> loop;
    std::shared_ptr<NNet::TSslContext> clientContext;
    std::shared_ptr<NNet::TSslContext> serverContext;
    std::function<void(const char*)> sslDebugLogFunc = [](const char* s) { std::cerr << s << "\n"; };

    if (ssl) {
        clientContext = std::shared_ptr<NNet::TSslContext>(new NNet::TSslContext(NNet::TSslContext::Client(sslDebugLogFunc)));
        serverContext = std::shared_ptr<NNet::TSslContext>(new NNet::TSslContext(NNet::TSslContext::Server("server.crt", "server.key", sslDebugLogFunc)));
    }

    for (auto& host : hosts) {
        if (!host) {
            std::cerr << "Empty host\n"; return 1;
        }
        if (host.Id == id) {
            myHost = host;
        } else {
            if (ssl) {
                nodes[host.Id] = std::make_shared<TNode<NNet::TSslSocket<TPoller::TSocket>>>(
                    [&](const NNet::TAddress& addr) {
                        return std::move(NNet::TSslSocket(std::move(TPoller::TSocket(addr, loop.Poller())), *clientContext.get()));
                    },
                    std::to_string(host.Id),
                    NNet::TAddress{host.Address, host.Port},
                    timeSource);
            } else {
                nodes[host.Id] = std::make_shared<TNode<TPoller::TSocket>>(
                    [&](const NNet::TAddress& addr) { return TPoller::TSocket(addr, loop.Poller()); },
                    std::to_string(host.Id),
                    NNet::TAddress{host.Address, host.Port},
                    timeSource);
            }
        }
    }

    if (!myHost) {
        std::cerr << "Host not found\n"; return 1;
    }

    std::shared_ptr<IRsm> rsm = std::make_shared<TDummyRsm>();
    auto raft = std::make_shared<TRaft>(std::make_shared<TState>(), myHost.Id, nodes);
    TPoller::TSocket socket(NNet::TAddress{myHost.Address, myHost.Port}, loop.Poller());
    socket.Bind();
    socket.Listen();
    if (ssl) {
        auto sslSocket = NNet::TSslSocket(std::move(socket), *serverContext.get());
        TRaftServer server(loop.Poller(), std::move(sslSocket), raft, rsm, nodes, timeSource);
        server.Serve();
        loop.Loop();
    } else {
        TRaftServer server(loop.Poller(), std::move(socket), raft, rsm, nodes, timeSource);
        server.Serve();
        loop.Loop();
    }
    return 0;
}
