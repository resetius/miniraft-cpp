#include <csignal>
#include <timesource.h>
#include <raft.h>
#include <server.h>

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    std::vector<THost> hosts;
    THost myHost;
    TNodeDict nodes;
    uint32_t id = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--node") && i < argc - 1) {
            // address:port:id
            hosts.push_back(THost{argv[++i]});
        } else if (!strcmp(argv[i], "--id") && i < argc - 1) {
            id = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--help")) {

        }
    }

    std::shared_ptr<ITimeSource> timeSource = std::make_shared<TTimeSource>();
    NNet::TLoop<NNet::TPoll> loop;

    for (auto& host : hosts) {
        if (host.Id == id) {
            myHost = host;
        } else {
            nodes[host.Id] = std::make_shared<TNode>(
                loop.Poller(),
                std::to_string(host.Id),
                NNet::TAddress{host.Address, host.Port},
                timeSource);
        }
    }

    auto raft = std::make_shared<TRaft>(myHost.Id, nodes, timeSource);
    TRaftServer server(loop.Poller(), NNet::TAddress{myHost.Address, myHost.Port}, raft, nodes, timeSource);
    server.Serve();
    loop.Loop();
    return 0;
}
