#include <sqlite3.h>
#include <iostream>

#include <miniraft/raft.h>
#include <miniraft/persist.h>
#include <miniraft/net/server.h>
#include "payload_validation.h"

#include "sql_rsm.h"

template<typename TPoller, typename TSocket>
NNet::TFuture<void> Client(TPoller& poller, TSocket socket) {
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
        while ((line = co_await lineReader.Read())) {
            std::string strLine;
            strLine += std::string_view(line.Part1.data(), line.Part1.size());
            strLine += std::string_view(line.Part2.data(), line.Part2.size());
            size_t pos = strLine.find(' ');
            auto prefix = strLine.substr(0, pos);
            TMessageHolder<TMessage> req;

            int flags = 0;
            if (!strcasecmp(prefix.data(), "create") || !strcasecmp(prefix.data(), "insert") || !strcasecmp(prefix.data(), "update")) {
                auto mes = NewHoldedMessage<TWriteSql>(sizeof(TWriteSql) + strLine.size());
                mes->Flags = TCommandRequest::EWrite;
                mes->QuerySize = strLine.size();
                memcpy(mes->Query, strLine.data(), strLine.size());
                req = mes;
            } else if (!strcasecmp(prefix.data(), "select")) {
                auto mes = NewHoldedMessage<TReadSql>(sizeof(TReadSql) + strLine.size());
                mes->QuerySize = strLine.size();
                memcpy(mes->Query, strLine.data(), strLine.size());
                req = mes;
            } else {
                std::cerr << "Cannot parse command: " << strLine << std::endl;
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
                std::cout << "commitIndex: " << res->Index << "\n";
                if (!data.empty()) std::cout << data << "\n";
            }
        }
    } catch (const std::exception& ex) {
        std::cout << "Exception: " << ex.what() << "\n";
    }
    co_return;
}

void usage(const char* prog) {
    std::cerr << prog << "--client|--server --id myid --node ip:port:id [--node ip:port:id ...]" << "\n";
    exit(0);
}

int main(int argc, char** argv)
{
    signal(SIGPIPE, SIG_IGN);
    std::vector<THost> hosts;
    THost myHost;
    TNodeDict nodes;
    uint32_t id = 0;
    bool server = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--server")) {
            server = true;
        } else if (!strcmp(argv[i], "--node") && i < argc - 1) {
            // address:port:id
            hosts.push_back(THost{argv[++i]});
        } else if (!strcmp(argv[i], "--id") && i < argc - 1) {
            id = atoi(argv[++i]);
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

        std::shared_ptr<IRsm> rsm = std::make_shared<TSql>("sql_file.db", myHost.Id);
        auto state = std::make_shared<TDiskState>("sql_state", myHost.Id);
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

        auto h = Client(loop.Poller(), std::move(socket));
        while (!h.done()) {
            loop.Step();
        }
    }
    return 0;
}

