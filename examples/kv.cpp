#include <string_view>

#include <miniraft/net/server.h>
#include <miniraft/persist.h>

#include "kv.h"

#include "kv_messages.h"

template<typename TPoller, typename TSocket>
NNet::TFuture<void> Client(TPoller& poller, TSocket socket, uint32_t flags) {
    using TFileHandle = typename TPoller::TFileHandle;
    TFileHandle input{0, poller}; // stdin
    co_await socket.Connect();
    std::cout << "Connected\n";

    NNet::TLine line;
    TCommandRequest header;
    header.Flags = TCommandRequest::EWrite;
    header.Type = static_cast<uint32_t>(TCommandRequest::MessageType);
    auto lineReader = NNet::TLineReader(input, 2*1024);
    auto byteWriter = NNet::TByteWriter(socket);
    const char* sep = " \t\r\n";

    try {
        std::cout << "> " << std::flush;
        while ((line = co_await lineReader.Read())) {
            std::string strLine;
            strLine += std::string_view(line.Part1.data(), line.Part1.size());
            strLine += std::string_view(line.Part2.data(), line.Part2.size());
            auto prefix = strtok((char*)strLine.data(), sep);
            if (!prefix) { std::cout << "> " << std::flush; continue; }
            TMessageHolder<TMessage> req;

            if (!strcmp(prefix, "set")) {
                auto key = strtok(nullptr, sep);
                auto val = strtok(nullptr, sep);
                if (!key || !val) { std::cout << "Expected set <key> <value>\n"; continue; }
                auto keySize = strlen(key);
                auto valSize = strlen(val);
                auto mes = NewHoldedMessage<TWriteKv>(sizeof(TWriteKv) + keySize + valSize);
                mes->Flags = TCommandRequest::EWrite;
                mes->KeySize = keySize;
                mes->ValSize = valSize;
                memcpy(mes->TKvEntry::Data, key, keySize);
                memcpy(mes->TKvEntry::Data+keySize, val, valSize);
                req = mes;
            } else if (!strcmp(prefix, "get")) {
                auto key = strtok(nullptr, sep);
                if (!key) { std::cout << "Expected a key\n"; continue; }
                auto size = strlen(key);
                auto mes = NewHoldedMessage<TReadKv>(sizeof(TReadKv) + size);
                mes->Flags = flags;
                mes->KeySize = size;
                memcpy(mes->Data, key, size);
                req = mes;
            } else if (!strcmp(prefix, "list")) {
                auto mes = NewHoldedMessage<TReadKv>(sizeof(TReadKv));
                mes->Flags = flags;
                req = mes;
            } else if (!strcmp(prefix, "del")) {
                auto key = strtok(nullptr, sep);
                if (!key) { std::cout << "Expected a key\n"; continue; }
                auto size = strlen(key);
                auto mes = NewHoldedMessage<TWriteKv>(sizeof(TWriteKv) + size);
                mes->Flags = TCommandRequest::EWrite;
                mes->KeySize = size;
                mes->ValSize = 0;
                memcpy(mes->TKvEntry::Data, key, size);
                req = mes;
            } else {
                std::cout << "Cannot parse command: " << strLine << "\n";
                continue;
            }

            co_await TMessageWriter(socket).Write(std::move(req));
            auto reply = co_await TMessageReader(socket).Read();
            auto res = reply.template Cast<TCommandResponse>();
            auto len = res->Len - sizeof(TCommandResponse);
            std::string_view data(res->Data, len);
            if (res->ErrorCode) {
                std::cout << "Request failed (error " << res->ErrorCode
                          << "). A write may still have committed.\n";
            } else {
                std::cout << "Ok, commitIndex: " << res->Index << "\n";
                if (!data.empty()) std::cout << data << "\n";
            }
            std::cout << "> " << std::flush;
        }
    } catch (const std::exception& ex) {
        std::cout << "Exception: " << ex.what() << "\n";
    }
    co_return;
}

void usage(const char* prog) {
    std::cerr << prog << "--client|--server --id myid --node ip:port:id [--node ip:port:id ...] [--persist] [--stale] [--consistent]" << "\n";
    exit(0);
}

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    std::vector<THost> hosts;
    THost myHost;
    TNodeDict nodes;
    uint32_t id = 0;
    bool server = false;
    bool persist = false;
    uint32_t flags = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--server")) {
            server = true;
        } else if (!strcmp(argv[i], "--node") && i < argc - 1) {
            // address:port:id
            hosts.push_back(THost{argv[++i]});
        } else if (!strcmp(argv[i], "--id") && i < argc - 1) {
            id = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--persist")) {
            persist = true;
        } else if (!strcmp(argv[i], "--stale")) {
            flags |= TCommandRequest::EStale;
        } else if (!strcmp(argv[i], "--consistent")) {
            flags |= TCommandRequest::EConsistent;
        } else if (!strcmp(argv[i], "--help")) {
            usage(argv[0]);
        }
    }

    using TPoller = NNet::TDefaultPoller;
    std::shared_ptr<ITimeSource> timeSource = std::make_shared<TTimeSource>();
    NNet::TLoop<TPoller> loop;

    if (server) {
        for (auto& host : hosts) {
            if (!host) {
                std::cerr << "Empty host\n"; return 1;
            }
            if (host.Id == id) {
                myHost = host;
            } else {
                nodes[host.Id] = std::make_shared<TNode<TPoller::TSocket>>(
                    [&](const NNet::TAddress& addr) { return TPoller::TSocket(addr, loop.Poller()); },
                    std::to_string(host.Id),
                    NNet::TAddress{host.Address, host.Port},
                    timeSource);
            }
        }

        if (!myHost) {
            std::cerr << "Host not found\n"; return 1;
        }

        std::shared_ptr<IRsm> rsm = std::make_shared<TKv>();
        std::shared_ptr<IState> state = std::make_shared<TState>();
        if (persist) {
            state = std::make_shared<TDiskState>("state", myHost.Id);
        }
        auto raft = std::make_shared<TRaft>(state, myHost.Id, nodes);
        TPoller::TSocket socket(NNet::TAddress{myHost.Address, myHost.Port}, loop.Poller());
        socket.Bind();
        socket.Listen();
        TRaftServer server(loop.Poller(), std::move(socket), raft, rsm, nodes, timeSource);
        server.Serve();
        loop.Loop();
    } else {
        NNet::TAddress addr{hosts[0].Address, hosts[0].Port};
        NNet::TSocket socket(std::move(addr), loop.Poller());

        auto h = Client(loop.Poller(), std::move(socket), flags);
        while (!h.done()) {
            loop.Step();
        }
    }

    return 0;
}
