#include "coroio/all.hpp"
#include <messages.h>
#include <coroio/socket.hpp>
#include <vector>
#include <server.h>

uint64_t inflight = 0;
uint64_t maxInflight = 128;

struct TLine {
    std::string_view Part1;
    std::string_view Part2;

    size_t Size() const {
        return Part1.size() + Part2.size();
    }

    operator bool() const {
        return !Part1.empty();
    }
};

struct TLineSplitter {
public:
    TLineSplitter(int maxLen)
        : WPos(0)
        , RPos(0)
        , Size(0)
        , Cap(maxLen * 2)
        , Data(Cap, 0)
        , View(Data)
    { }

    TLine Pop() {
        auto end = View.substr(RPos, Size);
        auto begin = View.substr(0, Size - end.size());

        auto p1 = end.find('\n');
        if (p1 == std::string_view::npos) {
            auto p2 = begin.find('\n');
            if (p2 == std::string_view::npos) {
                return {};
            }

            RPos = p2 + 1;
            Size -= end.size() + p2 + 1;
            return TLine { end, begin.substr(0, p2 + 1) };
        } else {
            RPos += p1 + 1;
            Size -= p1 + 1;
            return TLine { end.substr(0, p1 + 1), {} };
        }
    }

    void Push(const char* buf, size_t size) {
        if (Size + size > Data.size()) {
            throw std::runtime_error("Overflow");
        }

        auto first = std::min(size, Cap - WPos);
        memcpy(&Data[WPos], buf, first);
        memcpy(&Data[0], buf + first, std::max<size_t>(0, size - first));
        WPos = (WPos + size) % Cap;
        Size = Size + size;
    }

private:
    size_t WPos;
    size_t RPos;
    size_t Size;
    size_t Cap;
    std::string Data;
    std::string_view View;
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
    TLineSplitter splitter(2 * 1024);
    co_await socket.Connect();
    std::cout << "Connected\n";
    char buf[1024];
    ssize_t size = 1;

    auto reader = ClientReader(poller, socket);

    TLine line;
    TCommandRequest header;
    header.Type = static_cast<uint32_t>(TCommandRequest::MessageType);

    try {
        while (size && (size = co_await input.ReadSome(buf, sizeof(buf)))) {
            splitter.Push(buf, size);

            while ((line = splitter.Pop())) {
                while (inflight >= maxInflight) {
                    co_await poller.Sleep(std::chrono::milliseconds(0));
                }

                inflight++;

                //std::cout << "Sending " << line.Size() << " bytes: '" << line.Part1 << line.Part2 << "'\n";
                std::cout << "Sending\n";
                header.Len = sizeof(header) + line.Size();
                co_await NNet::TByteWriter(socket).Write(&header, sizeof(header));
                co_await NNet::TByteWriter(socket).Write(line.Part1.data(), line.Part1.size());
                co_await NNet::TByteWriter(socket).Write(line.Part2.data(), line.Part2.size());
            }
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
    using TPoller = NNet::TDefaultPoller;
    std::shared_ptr<ITimeSource> timeSource = std::make_shared<TTimeSource>();
    NNet::TLoop<TPoller> loop;
    Client(loop.Poller(), NNet::TAddress{hosts[0].Address, hosts[0].Port});
    loop.Loop();
    return 0;
}
