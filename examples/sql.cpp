#include <sqlite3.h>
#include <iostream>

#include <raft.h>
#include <persist.h>
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

struct TRow {
    std::vector<std::optional<std::string>> Values;
};

struct TResult {
    std::vector<std::string> Cols;
    std::vector<TRow> Rows;

    void Clear() {
        Cols.clear();
        Rows.clear();
    }

    bool Empty() {
        return Cols.empty();
    }
};

class TSql: public IRsm {
public:
    TSql(const std::string& path, int serverId);
    ~TSql();

    // select
    TMessageHolder<TMessage> Read(TMessageHolder<TCommandRequest> message, uint64_t index) override;
    // insert, update, create
    TMessageHolder<TMessage> Write(TMessageHolder<TLogEntry> message, uint64_t index) override;
    // convert request to log message
    TMessageHolder<TLogEntry> Prepare(TMessageHolder<TCommandRequest> message) override;

private:
    bool Execute(const std::string& q);
    static int Process(void* self, int ncols, char** values, char** cols);
    TMessageHolder<TMessage> Reply(const std::string& ans, uint64_t index);

    TResult Result;
    std::string LastError;
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
    Execute(R"__(CREATE TABLE IF NOT EXISTS raft_metadata_ (key TEXT PRIMARY KEY, value TEXT))__");
    Execute(R"__(SELECT value FROM raft_metadata_ WHERE key = 'LastAppliedIndex')__");
    if (!Result.Empty()) {
        LastAppliedIndex = std::stoi(*Result.Rows[0].Values[0]);
    }
    std::cerr << "LastAppliedIndex: " << LastAppliedIndex << std::endl;
}

TSql::~TSql()
{
    if (Db) {
        if (sqlite3_close(Db) != SQLITE_OK) {
            std::cerr << "Failed to close db, error:" << sqlite3_errmsg(Db) << std::endl; 
        }
    }
}

int TSql::Process(void* self, int ncols, char** values, char** cols) {
    TSql* this_ = (TSql*)self;
    if (this_->Result.Cols.empty()) {
        for (int i = 0; i < ncols; i++) {
            this_->Result.Cols.emplace_back(cols[i]);
        }
    }
    TRow row;
    for (int i = 0; i < ncols; i++) {
        if (values[i]) {
            row.Values.emplace_back(values[i]);
        } else {
            row.Values.emplace_back(std::nullopt);
        }
    }
    this_->Result.Rows.emplace_back(std::move(row));
    return 0;
}

bool TSql::Execute(const std::string& q) {
    char* err = nullptr;
    std::cerr << "Execute: " << q << std::endl;
    Result.Clear();
    LastError.clear();
    if (sqlite3_exec(Db, q.c_str(), Process, this, &err) != SQLITE_OK) {
        std::cerr << "Cannot apply changes, error: " << err << std::endl;
        LastError = err;
        sqlite3_free(err);
        return false;
    }
    std::cerr << "OK" << std::endl;
    return true;
}
 
TMessageHolder<TMessage> TSql::Read(TMessageHolder<TCommandRequest> message, uint64_t index) {
    auto readSql = message.Cast<TReadSql>();
    if (!Execute(std::string(readSql->Query, readSql->QuerySize))) {
        return Reply(LastError, index);
    } else {
        std::string text;
        for (int i = 0; i < Result.Cols.size(); i++) {
            text += Result.Cols[i];
            if (i != Result.Cols.size() - 1) {
                text += ",";
            }
        }
        text += "\n";
        for (int j = 0; j < Result.Rows.size(); j++) {
            for (int i = 0; i < Result.Cols.size(); i++) {
                text += Result.Rows[j].Values[i] ? *Result.Rows[j].Values[i] : "null";
                if (i != Result.Cols.size() - 1) {
                    text += ",";
                }
            }
            text += "\n";
        }
        return Reply(text, index);
    }
}

TMessageHolder<TMessage> TSql::Write(TMessageHolder<TLogEntry> message, uint64_t index) {
    // TODO: index + 1 == LastAppliedIndex

    std::string err;
    if (LastAppliedIndex < index) {
        auto entry = message.Cast<TSqlLogEntry>();
        std::cerr << "Execute write of size: " << entry->QuerySize << std::endl;
        std::string updateLastApplied;
        updateLastApplied += "INSERT INTO raft_metadata_ (key, value) VALUES ('LastAppliedIndex','" + std::to_string(index) + "')\n";
        updateLastApplied += "ON CONFLICT(key) DO UPDATE SET value = '" + std::to_string(index) + "';\n";
        std::string q = "BEGIN TRANSACTION;\n";
        q += std::string(entry->Query, entry->QuerySize);
        if (q.back() != ';') {
            q += ";\n";
        }
        q += updateLastApplied;
        q += "COMMIT;";
        if (Execute(q)) {
            LastAppliedIndex = index;
        } else {
            err = LastError;
            Execute("ROLLBACK;");
            Execute(updateLastApplied); // need to update LastAppliedIndex in order not to execute failed query aqain
        }
    }
    return Reply(err, index);
}

TMessageHolder<TMessage> TSql::Reply(const std::string& ans, uint64_t index)
{
    auto res = NewHoldedMessage<TCommandResponse>(sizeof(TCommandResponse)+ans.size());
    res->Index = index;
    memcpy(res->Data, ans.data(), ans.size());
    return res;
}

TMessageHolder<TLogEntry> TSql::Prepare(TMessageHolder<TCommandRequest> command) {
    auto dataSize = command->Len - sizeof(TCommandRequest);
    auto entry = NewHoldedMessage<TLogEntry>(sizeof(TLogEntry)+dataSize);
    memcpy(entry->Data, command->Data, dataSize);
    return entry;
}

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
            std::cout << "commitIndex: " << res->Index << "\n";
            if (!data.empty()) {
                std::cout << data << "\n";
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
        auto raft = std::make_shared<TRaft>(rsm, state, myHost.Id, nodes);
        TPoller::TSocket socket(NNet::TAddress{myHost.Address, myHost.Port}, loop.Poller());
        socket.Bind();
        socket.Listen();
        TRaftServer server(loop.Poller(), std::move(socket), raft, nodes, timeSource);
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

