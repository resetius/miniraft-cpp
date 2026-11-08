#pragma once
#include <sqlite3.h>
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

struct TSqlReadLimits {
    // Approximate SQLite VM instruction budget, checked at intervals <= 1000.
    uint32_t MaxVmSteps = 1'000'000;
    size_t MaxResultBytes = TMessageLimits{}.MaxFrameBytes - sizeof(TCommandResponse);
};

class TSql: public IRsm {
public:
    void Validate(TMessageHolder<TCommandRequest> message) const override;
    TSql(const std::string& path, int serverId, TSqlReadLimits readLimits = {});
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
    std::string ExecuteRead(const std::string& q);
    TMessageHolder<TMessage> Reply(const std::string& ans, uint64_t index);

    TSqlReadLimits ReadLimits;
    std::string LastError;
    sqlite3* Db = nullptr;
};

// Whether a failed SQL write fails the same way on every replica.
bool IsDeterministicSqlError(int code);

