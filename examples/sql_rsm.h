#pragma once
#include <sqlite3.h>
#include <optional>
#include <miniraft/raft.h>

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
    void Validate(TMessageHolder<TCommandRequest> message) const override;
    TSql(const std::string& path, int serverId);
    ~TSql();

    // select
    TMessageHolder<TMessage> Read(TMessageHolder<TCommandRequest> message, uint64_t index) override;
    // insert, update, create
    TMessageHolder<TMessage> Write(TMessageHolder<TLogEntry> message, uint64_t index) override;
    // convert request to log message
    TMessageHolder<TLogEntry> Prepare(TMessageHolder<TCommandRequest> message) override;

private:
    int Execute(const std::string& q);
    void RequireExecute(const std::string& q);
    void ExecuteRead(const std::string& q);
    static int Process(void* self, int ncols, char** values, char** cols);
    TMessageHolder<TMessage> Reply(const std::string& ans, uint64_t index);

    TResult Result;
    std::string LastError;
    sqlite3* Db = nullptr;
};

