#include <messages.h>
#include <socket.hpp>
#include <vector>
#include <server.h>

uint64_t inflight = 0;
uint64_t maxInflight = 1;

struct TLineSplitter {
public:
    std::string Pop() {
        size_t p = data.find('\n');
        std::string line;
        if (p != std::string::npos) {
            line = data.substr(0, p+1);
            data = data.substr(p+1);
        }
        return line;
    }

    void Push(const char* buf, ssize_t size) {
        data += std::string_view(buf, size);
    }

private:
    std::string data;
};

template<typename Poller>
NNet::TTestTask ClientReader(Poller& poller, typename Poller::TSocket& socket) {
    try {
        while (true) {
            auto response = co_await TReader(socket).Read();
            auto commandResponse = response.template Cast<TCommandResponse>();
            std::cout << "Ok, commitIndex: " << commandResponse->Index << "\n";
            inflight --;
        }
    } catch (const std::exception& ex) {
        std::cout << "Exception: " << ex.what() << "\n";
    }
    co_return;
}

template<typename Poller>
NNet::TSimpleTask Client(Poller& poller, NNet::TAddress addr) {
    typename Poller::TSocket socket(std::move(addr), poller);
    NNet::TSocket input{NNet::TAddress{}, 0, poller}; // stdin
    TLineSplitter splitter;
    co_await socket.Connect();
    std::cout << "Connected\n";
    char buf[1024];
    std::string line;
    ssize_t size = 1;

    auto reader = ClientReader(poller, socket);

    try {
        while (size && (size = co_await input.ReadSome(buf, sizeof(buf)))) {
            splitter.Push(buf, size);
            auto line = splitter.Pop();
            if (line.empty()) {
                continue;
            }
            auto mes = NewHoldedMessage<TCommandRequest>(
                sizeof(TCommandRequest) + line.size()
            );
            memcpy(mes->Data, line.c_str(), line.size());
            std::cout << "Sending\n";
            while (inflight >= maxInflight) {
                co_await poller.Sleep(std::chrono::milliseconds(0));
            }
            inflight++;
            co_await TWriter(socket).Write(std::move(mes));
        }
    } catch (const std::exception& ex) {
        std::cout << "Exception: " << ex.what() << "\n";
    }
    reader.destroy();
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
