#include <sqlite3.h>
#include <iostream>

#include <raft.h>
#include <server.h>

struct TSqlEntry {
    uint32_t QuerySize = 0;
    char Query[0];
};

struct TSqlLogEntry: public TLogEntry, public TSqlEntry
{
};

struct TWriteSql: public TCommandRequest, public TSqlEntry
{
};

struct TReadSql: public TCommandRequest, public TSqlEntry 
{
};

class TSql: public IRsm {
public:
    TSql(const std::string& path, int serverId);
    ~TSql();

    // select
    TMessageHolder<TMessage> Read(TMessageHolder<TCommandRequest> message, uint64_t index) override;
    // insert, update, create
    // TODO: implement result
    void Write(TMessageHolder<TLogEntry> message, uint64_t index) override;
    // convert request to log message
    TMessageHolder<TLogEntry> Prepare(TMessageHolder<TCommandRequest> message, uint64_t term) override;

private:
    void Execute(const std::string& q);

    uint64_t LastAppliedIndex = 0;
    sqlite3* Db = nullptr;
};

TSql::TSql(const std::string& path, int serverId)
{
    std::string dbPath = path + "." + std::to_string(serverId);
    if (sqlite3_open(dbPath.c_str(), &Db) != SQLITE_OK) {
        std::cerr << "Cannot open db: `" << dbPath << "', " 
            << "error: " << sqlite3_errmsg(Db)
            << std::endl;
        throw std::runtime_error("Cannot open db");
    }
}

TSql::~TSql()
{
    if (Db) {
        if (sqlite3_close(Db) != SQLITE_OK) {
            std::cerr << "Failed to close db, error:" << sqlite3_errmsg(Db) << std::endl; 
        }
    }
}

void TSql::Execute(const std::string& q) {
    char* err = nullptr;
    if (sqlite3_exec(Db, q.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        // TODO: Return error to user
        // TODO: Move LastApplied?
        std::cerr << "Cannot apply changes, error: " << err << std::endl;
        throw std::runtime_error("Failed to execute SQL command");
    }
}
 
TMessageHolder<TMessage> TSql::Read(TMessageHolder<TCommandRequest> message, uint64_t index) {
    // TODO: non-empty result
    auto readSql = message.Cast<TReadSql>();
    Execute(std::string(readSql->Query, readSql->QuerySize));
    return NewHoldedMessage<TCommandResponse>(sizeof(TCommandResponse));
}

void TSql::Write(TMessageHolder<TLogEntry> message, uint64_t index) {
    // TODO: index + 1 == LastAppliedIndex
    if (index < LastAppliedIndex) {
        auto entry = message.Cast<TSqlLogEntry>();
        Execute(std::string(entry->Query, entry->QuerySize));
        // TODO: must be committed with query at the same tx
        LastAppliedIndex = index;
    }
}

TMessageHolder<TLogEntry> TSql::Prepare(TMessageHolder<TCommandRequest> command, uint64_t term) {
    auto dataSize = command->Len - sizeof(TCommandRequest);
    auto entry = NewHoldedMessage<TLogEntry>(sizeof(TLogEntry)+dataSize);
    memcpy(entry->Data, command->Data, dataSize);
    entry->Term = term;
    return entry;
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
        auto raft = std::make_shared<TRaft>(rsm, myHost.Id, nodes);
        TPoller::TSocket socket(NNet::TAddress{myHost.Address, myHost.Port}, loop.Poller());
        socket.Bind();
        socket.Listen();
        TRaftServer server(loop.Poller(), std::move(socket), raft, nodes, timeSource);
        server.Serve();
        loop.Loop();
    } else {
        // TODO: client
    }
    return 0;
}

