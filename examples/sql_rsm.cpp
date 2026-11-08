#include "sql_rsm.h"
#include "payload_validation.h"
#include <iostream>

void TSql::Validate(TMessageHolder<TCommandRequest> message) const {
    auto query = message.Cast<TReadSql>();
    ValidatePayloadRange(message, query->Query, query->QuerySize);
}

TSql::TSql(const std::string& path, int serverId)
{
    std::string dbPath = path == ":memory:" ? path : path + "." + std::to_string(serverId);
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
    ValidatePayloadRange(message, readSql->Query, readSql->QuerySize);
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
        ValidatePayloadRange(message, entry->Query, entry->QuerySize);
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
    if (ans.size() > TMessageLimits{}.MaxFrameBytes - sizeof(TCommandResponse))
        throw TMessageError("SQL response exceeds frame limit");
    auto res = NewHoldedMessage<TCommandResponse>(sizeof(TCommandResponse)+ans.size());
    res->Index = index;
    memcpy(res->Data, ans.data(), ans.size());
    return res;
}

TMessageHolder<TLogEntry> TSql::Prepare(TMessageHolder<TCommandRequest> command) {
    auto write = command.Cast<TWriteSql>();
    ValidatePayloadRange(command, write->Query, write->QuerySize);
    auto dataSize = command->Len - sizeof(TCommandRequest);
    auto entry = NewHoldedMessage<TLogEntry>(sizeof(TLogEntry)+dataSize);
    memcpy(entry->Data, command->Data, dataSize);
    return entry;
}

