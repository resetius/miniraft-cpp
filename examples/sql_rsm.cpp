#include "sql_rsm.h"
#include "payload_validation.h"
#include <iostream>
#include <climits>
#include <charconv>

namespace {
int ReadAuthorization(void*, int action, const char*, const char* second, const char*, const char*) {
    if (action == SQLITE_SELECT || action == SQLITE_READ || action == SQLITE_RECURSIVE) return SQLITE_OK;
    if (action == SQLITE_FUNCTION && second) {
        // Keep this allowlist limited to trusted SQLite built-ins. In particular,
        // readonly statements can still call extension functions with side effects.
        for (const char* function : {"abs", "avg", "coalesce", "count", "glob", "hex", "ifnull",
             "instr", "length", "like", "lower", "ltrim", "max", "min", "nullif", "quote",
             "replace", "round", "rtrim", "substr", "substring", "sum", "total", "trim",
             "typeof", "unicode", "upper", "group_concat"})
            if (sqlite3_stricmp(function, second) == 0) return SQLITE_OK;
    }
    return SQLITE_DENY;
}
int WriteAuthorization(void*, int action, const char* first, const char* second, const char* database, const char* context) {
    const auto metadata = [](const char* name) {
        return name && sqlite3_stricmp(name, "raft_metadata_") == 0;
    };
    if (metadata(first) || ((action == SQLITE_CREATE_TRIGGER || action == SQLITE_DROP_TRIGGER ||
        action == SQLITE_ALTER_TABLE || action == SQLITE_CREATE_INDEX || action == SQLITE_DROP_INDEX) && metadata(second))) return SQLITE_DENY;
    if (database && sqlite3_stricmp(database, "main") != 0) return SQLITE_DENY;
    switch (action) {
    case SQLITE_INSERT: case SQLITE_UPDATE: case SQLITE_DELETE:
    case SQLITE_CREATE_TABLE: case SQLITE_CREATE_INDEX: case SQLITE_CREATE_VIEW: case SQLITE_CREATE_TRIGGER:
    case SQLITE_DROP_TABLE: case SQLITE_DROP_INDEX: case SQLITE_DROP_VIEW: case SQLITE_DROP_TRIGGER:
    case SQLITE_REINDEX: case SQLITE_ANALYZE:
        return SQLITE_OK;
    default:
        return ReadAuthorization(nullptr, action, first, second, database, context);
    }
}
using TAuthorizer = int (*)(void*, int, const char*, const char*, const char*, const char*);
struct TSqlPolicy {
    sqlite3* Db;
    explicit TSqlPolicy(sqlite3* db, TAuthorizer authorize) : Db(db) {
        if (sqlite3_set_authorizer(Db, authorize, nullptr) != SQLITE_OK)
            throw TMessageError("Cannot install SQL policy");
    }
    ~TSqlPolicy() { sqlite3_set_authorizer(Db, nullptr, nullptr); }
};
struct TTransactionCleanup {
    sqlite3* Db;
    ~TTransactionCleanup() {
        if (!sqlite3_get_autocommit(Db)) sqlite3_exec(Db, "ROLLBACK", nullptr, nullptr, nullptr);
    }
};
using TStatement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
}

void TSql::ExecuteRead(const std::string& q) {
    if (q.size() > INT_MAX || q.find('\0') != std::string::npos)
        throw TMessageError("Invalid SQL read text");
    TSqlPolicy policy(Db, ReadAuthorization); // retained through prepare, stepping and finalization
    sqlite3_stmt* raw = nullptr;
    const char* tail = nullptr;
    int rc = sqlite3_prepare_v2(Db, q.data(), int(q.size()), &raw, &tail);
    TStatement statement(raw, sqlite3_finalize);
    if (rc != SQLITE_OK || !statement || !sqlite3_stmt_readonly(statement.get()))
        throw TMessageError("SQL read must be one permitted read-only statement");
    raw = nullptr;
    rc = sqlite3_prepare_v2(Db, tail, int(q.data() + q.size() - tail), &raw, nullptr);
    TStatement extra(raw, sqlite3_finalize);
    if (rc != SQLITE_OK || extra) throw TMessageError("Multiple SQL read statements are not permitted");
    Result.Clear();
    LastError.clear();
    while ((rc = sqlite3_step(statement.get())) == SQLITE_ROW) {
        const int count = sqlite3_column_count(statement.get());
        std::vector<char*> values(count), columns(count);
        for (int i = 0; i < count; ++i) {
            values[i] = const_cast<char*>(reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), i)));
            columns[i] = const_cast<char*>(sqlite3_column_name(statement.get(), i));
        }
        Process(this, count, values.data(), columns.data());
    }
    if (rc != SQLITE_DONE) throw TMessageError("SQL read execution failed");
}

void TSql::Validate(TMessageHolder<TCommandRequest> message) const {
    auto query = message.Cast<TReadSql>();
    ValidatePayloadRange(message, query->Query, query->QuerySize);
    if (memchr(query->Query, 0, query->QuerySize)) throw TMessageError("Invalid SQL query text");
}

TSql::TSql(const std::string& path, int serverId)
{
    std::string dbPath = path == ":memory:" ? path : path + "." + std::to_string(serverId);
    const int opened = sqlite3_open(dbPath.c_str(), &Db);
    // Constructors that fail must also close the connection and release its locks.
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> cleanup(Db, sqlite3_close);
    if (opened != SQLITE_OK) throw std::runtime_error("Cannot open SQL database");
    RequireExecute("CREATE TABLE IF NOT EXISTS raft_metadata_ (key TEXT PRIMARY KEY, value TEXT)");
    sqlite3_stmt* raw = nullptr;
    const int prepared = sqlite3_prepare_v2(Db,
        "SELECT value FROM raft_metadata_ WHERE key='LastAppliedIndex'", -1, &raw, nullptr);
    TStatement statement(raw, sqlite3_finalize);
    if (prepared != SQLITE_OK || !statement) throw std::runtime_error("Cannot read SQL application progress");
    int rc = sqlite3_step(statement.get());
    if (rc == SQLITE_ROW) {
        if (sqlite3_column_type(statement.get(), 0) != SQLITE_TEXT)
            throw std::runtime_error("Invalid SQL application progress");
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 0));
        if (!text) throw std::runtime_error("Cannot read SQL application progress");
        const auto length = sqlite3_column_bytes(statement.get(), 0);
        uint64_t index = 0;
        const auto parsed = std::from_chars(text, text + length, index);
        if (parsed.ec != std::errc{} || parsed.ptr != text + length)
            throw std::runtime_error("Invalid SQL application progress");
        LastAppliedIndex = index;
        rc = sqlite3_step(statement.get());
    }
    if (rc != SQLITE_DONE) throw std::runtime_error("Invalid SQL application progress");
    cleanup.release();
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

int TSql::Execute(const std::string& q) {
    char* err = nullptr;
    Result.Clear();
    LastError.clear();
    const int rc = sqlite3_exec(Db, q.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) LastError = err ? err : sqlite3_errmsg(Db);
    sqlite3_free(err);
    return rc;
}

void TSql::RequireExecute(const std::string& q) {
    if (Execute(q) != SQLITE_OK) throw std::runtime_error("SQL state update failed: " + LastError);
}

TMessageHolder<TMessage> TSql::Read(TMessageHolder<TCommandRequest> message, uint64_t index) {
    auto readSql = message.Cast<TReadSql>();
    ValidatePayloadRange(message, readSql->Query, readSql->QuerySize);
    ExecuteRead(std::string(readSql->Query, readSql->QuerySize));
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

TMessageHolder<TMessage> TSql::Write(TMessageHolder<TLogEntry> message, uint64_t index) {
    if (index <= LastAppliedIndex) return Reply({}, index);
    auto entry = message.Cast<TSqlLogEntry>();
    ValidatePayloadRange(message, entry->Query, entry->QuerySize);
    const std::string query(entry->Query, entry->QuerySize);
    if (query.find('\0') != std::string::npos) throw TMessageError("Invalid SQL write text");
    if (!sqlite3_get_autocommit(Db)) throw std::runtime_error("SQL application transaction already active");
    TTransactionCleanup transaction{Db};
    RequireExecute("BEGIN IMMEDIATE");
    int rc;
    std::string error;
    {
        // Only the payload runs under this policy. It cannot end the transaction
        // or access the progress table, including through views and triggers.
        TSqlPolicy policy(Db, WriteAuthorization);
        rc = Execute(query);
        error = LastError;
    }
    if (rc != SQLITE_OK) {
        if (!sqlite3_get_autocommit(Db)) RequireExecute("ROLLBACK");
        switch (rc & 0xff) {
        case SQLITE_ERROR: case SQLITE_AUTH: case SQLITE_CONSTRAINT: case SQLITE_MISMATCH:
            break; // SQL/application rejection: consume this entry without its effects.
        default:
            throw std::runtime_error("SQL application execution failed: " + error);
        }
        // Some errors (including OR ROLLBACK) already rolled back the transaction.
        // Start a new one to record the rejected entry; never ignore failure here.
        RequireExecute("BEGIN IMMEDIATE");
    }
    const auto progress = std::to_string(index);
    RequireExecute("INSERT INTO raft_metadata_ (key, value) VALUES ('LastAppliedIndex','" + progress +
        "') ON CONFLICT(key) DO UPDATE SET value='" + progress + "'");
    RequireExecute("COMMIT");
    LastAppliedIndex = index;
    return Reply(error, index);
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
    if (memchr(write->Query, 0, write->QuerySize)) throw TMessageError("Invalid SQL write text");
    auto dataSize = command->Len - sizeof(TCommandRequest);
    auto entry = NewHoldedMessage<TLogEntry>(sizeof(TLogEntry)+dataSize);
    memcpy(entry->Data, command->Data, dataSize);
    return entry;
}

