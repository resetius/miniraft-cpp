#include <messages.h>
#include <socket.hpp>
#include <vector>
#include <server.h>

template<typename Poller>
NNet::TSimpleTask Client(Poller& poller, NNet::TAddress addr) {
    typename Poller::TSocket socket(std::move(addr), poller);
    co_await socket.Connect();
    std::cout << "Connected\n";
    char buf[1024];
    while (fgets(buf, sizeof(buf), stdin)) {
        auto len = strlen(buf);
        auto mes = NewHoldedMessage<TCommandRequest>(
            sizeof(TCommandRequest) + len
        );
        memcpy(mes->Data, buf, len);
        std::cout << "Sending\n";
        co_await TWriter(socket).Write(std::move(mes));
        auto response = co_await TReader(socket).Read();
        auto commandResponse = response.template Cast<TCommandResponse>();
        std::cout << "Ok, commitIndex: " << commandResponse->Index << "\n";
    }
    co_return;
}

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    std::vector<THost> hosts;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--node") && i < argc - 1) {
            // address:port:id
            hosts.push_back(THost{argv[++i]});
        } else if (!strcmp(argv[i], "--help")) {

        }
    }
    std::shared_ptr<ITimeSource> timeSource = std::make_shared<TTimeSource>();
    NNet::TLoop<NNet::TPoll> loop;
    Client(loop.Poller(), NNet::TAddress{hosts[0].Address, hosts[0].Port});
    loop.Loop();
    return 0;
}
