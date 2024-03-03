#include <coroio/ssl.hpp>
#include <coroio/socket.hpp>

#include <messages.h>
#include <server.h>

#include <vector>
#include <queue>

uint64_t inflight = 0;
uint64_t maxInflight = 128;
std::queue<ITimeSource::Time> times;
TTimeSource timeSource;

using namespace NNet;

template<typename TPoller, typename TSocket>
TVoidSuspendedTask ClientReader(TPoller& poller, TSocket& socket) {
    try {
        while (true) {
            auto response = co_await TMessageReader(socket).Read();
            auto t = times.front(); times.pop();
            auto dt = timeSource.Now() - t;
            auto commandResponse = response.template Cast<TCommandResponse>();
            auto len = commandResponse->Len - sizeof(TCommandResponse);
            std::string_view data(commandResponse->Data, len);
            std::cout << "Ok, commitIndex: " << commandResponse->Index << " "
                      << data << " " << dt.count() << "\n";
            inflight --;
        }
    } catch (const std::exception& ex) {
        std::cout << "ClientReader Exception: " << ex.what() << "\n";
    }
    co_return;
}

template<typename TPoller, typename TSocket>
TVoidTask Client(TPoller& poller, TSocket socket) {
    using TFileHandle = typename TPoller::TFileHandle;
    TFileHandle input{0, poller}; // stdin
    co_await socket.Connect();
    std::cout << "Connected\n";
    char buf[1024];
    ssize_t size = 1;

    auto reader = ClientReader(poller, socket);

    TLine line;
    TCommandRequest header;
    header.Flags = TCommandRequest::EWrite;
    header.Type = static_cast<uint32_t>(TCommandRequest::MessageType);
    auto lineReader = TLineReader(input, 2*1024);
    auto byteWriter = TByteWriter(socket);

    try {
        while ((line = co_await lineReader.Read())) {
            while (inflight >= maxInflight) {
                co_await poller.Yield();
            }

            inflight++;

            //std::cout << "Sending " << line.Size() << " bytes: '" << line.Part1 << line.Part2 << "'\n";
            //std::cout << "Sending\n";
            header.Len = sizeof(header) + line.Size();
            header.Flags = TCommandRequest::EWrite;
            times.push(timeSource.Now());
            if (line.Part1.data()[0] == '_') {
                // read
                header.Flags = 0;
                header.Len = sizeof(header) + sizeof(uint64_t);
                co_await byteWriter.Write(&header, sizeof(header));
                uint64_t index;
                std::string strIndex;
                strIndex += std::string_view(line.Part1.data(), line.Part1.size());
                strIndex += std::string_view(line.Part2.data(), line.Part2.size());
                sscanf(strIndex.data(), "_ %llu", &index);
                co_await byteWriter.Write(&index, sizeof(index));
            } else {
                co_await byteWriter.Write(&header, sizeof(header));
                co_await byteWriter.Write(line.Part1.data(), line.Part1.size());
                co_await byteWriter.Write(line.Part2.data(), line.Part2.size());
            }
        }
    } catch (const std::exception& ex) {
        std::cout << "Exception: " << ex.what() << "\n";
    }
    reader.destroy();
    co_return;
}

template<typename TPoller, typename TSocket>
TVoidTask SslClient(TPoller& poller, TSocket socket) {
    using TFileHandle = typename TPoller::TFileHandle;
    TFileHandle input{0, poller}; // stdin
    co_await socket.Connect();
    std::cout << "Connected\n";
    char buf[1024];
    ssize_t size = 1;

    auto reader = ClientReader(poller, socket);

    TLine line;
    TCommandRequest header;
    header.Type = static_cast<uint32_t>(TCommandRequest::MessageType);
    auto lineReader = TLineReader(input, 2*1024);
    auto byteWriter = TByteWriter(socket);

    try {
        while ((line = co_await lineReader.Read())) {
            //std::cout << "Sending " << line.Size() << " bytes: '" << line.Part1 << line.Part2 << "'\n";
            //std::cout << "Sending\n";
            header.Len = sizeof(header) + line.Size();
            times.push(timeSource.Now());
            co_await byteWriter.Write(&header, sizeof(header));
            co_await byteWriter.Write(line.Part1.data(), line.Part1.size());
            co_await byteWriter.Write(line.Part2.data(), line.Part2.size());

            auto response = co_await TMessageReader(socket).Read();
            auto t = times.front(); times.pop();
            auto dt = timeSource.Now() - t;
            auto commandResponse = response.template Cast<TCommandResponse>();
            std::cout << "Ok, commitIndex: " << commandResponse->Index << " " << dt.count() << "\n";
        }
    } catch (const std::exception& ex) {
        std::cout << "Exception: " << ex.what() << "\n";
    }
    reader.destroy();
    co_return;
}

void usage(const char* prog) {
    std::cerr << prog << " --node ip:port:id\n";
    exit(0);
}

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    std::vector<THost> hosts;
    bool ssl = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--node") && i < argc - 1) {
            // address:port:id
            hosts.push_back(THost{argv[++i]});
        } else if (!strcmp(argv[i], "--ssl")) {
            ssl = true;
        } else if (!strcmp(argv[i], "--help")) {
            usage(argv[0]);
        }
    }

    if (hosts.empty() || !hosts[0]) {
        std::cerr << "At least one node must be set\n"; return 1;
    }
    using TPoller = NNet::TDefaultPoller;
    std::shared_ptr<ITimeSource> timeSource = std::make_shared<TTimeSource>();
    NNet::TLoop<TPoller> loop;
    NNet::TAddress addr{hosts[0].Address, hosts[0].Port};
    TSocket socket(std::move(addr), loop.Poller());
    if (ssl) {
        std::function<void(const char*)> sslDebugLogFunc = [](const char* s) { std::cerr << s << "\n"; };
        TSslContext ctx = TSslContext::Client(sslDebugLogFunc);
        TSslSocket sslSocket(std::move(socket), ctx);
        SslClient(loop.Poller(), std::move(sslSocket));
        loop.Loop();
    } else {
        Client(loop.Poller(), std::move(socket));
        loop.Loop();
    }
    return 0;
}
