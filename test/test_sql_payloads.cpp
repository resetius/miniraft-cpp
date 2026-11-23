#include "../examples/sql_rsm.h"
#include <miniraft/persist.h>
#include <filesystem>
#include <chrono>
#include <gtest/gtest.h>
namespace {
TMessageHolder<TReadSql> Query(const std::string& text) {
    auto message = NewHoldedMessage<TReadSql>(sizeof(TReadSql) + text.size());
    message->QuerySize = text.size();
    memcpy(message->Query, text.data(), text.size());
    return message;
}
std::string ReadText(TSql& sql, const std::string& query) {
    auto reply = sql.Read(Query(query), sql.LastAppliedIndex).Cast<TCommandResponse>();
    return std::string(reply->Data, reply->Len - sizeof(TCommandResponse));
}
std::string Apply(TSql& sql, const std::string& query, uint64_t index) {
    auto command = Query(query);
    command->Flags = TCommandRequest::EWrite;
    auto reply = sql.Write(sql.Prepare(command), index).Cast<TCommandResponse>();
    return std::string(reply->Data, reply->Len - sizeof(TCommandResponse));
}
void Seed(TSql& sql) { Apply(sql, "CREATE TABLE t (value TEXT); INSERT INTO t VALUES ('original');", 1); }
template<class F> void Reject(F action) {
    bool rejected = false;
    try { action(); } catch (const TMessageError&) { rejected = true; }
    ASSERT_TRUE(rejected);
}
TEST(SqlPayloads, sql_prepare_rejects_short_query) {
    TSql sql(":memory:", 1);
    auto command = NewHoldedMessage<TWriteSql>();
    command->QuerySize = UINT32_MAX;
    Reject([&] { sql.Prepare(command); });
}
TEST(SqlPayloads, sql_read_rejects_short_query) {
    TSql sql(":memory:", 1);
    auto command = NewHoldedMessage<TReadSql>();
    command->QuerySize = UINT32_MAX;
    Reject([&] { sql.Read(command, 0); });
}
TEST(SqlPayloads, sql_apply_rejects_before_progress) {
    TSql sql(":memory:", 1);
    auto entry = NewHoldedMessage<TSqlLogEntry>();
    entry->QuerySize = UINT32_MAX;
    Reject([&] { sql.Write(entry, 1); });
    ASSERT_EQ(sql.LastAppliedIndex, 0);
}
TEST(SqlPayloads, valid_sql_payload_roundtrip) {
    TSql sql(":memory:", 1);
    const std::string query = "CREATE TABLE test (value TEXT); INSERT INTO test VALUES ('ok');";
    auto command = NewHoldedMessage<TWriteSql>(sizeof(TWriteSql) + query.size());
    command->QuerySize = query.size();
    memcpy(command->Query, query.data(), query.size());
    sql.Write(sql.Prepare(command), 1);
    ASSERT_EQ(sql.LastAppliedIndex, 1);
    const std::string select = "SELECT value FROM test";
    auto read = NewHoldedMessage<TReadSql>(sizeof(TReadSql) + select.size());
    read->QuerySize = select.size();
    memcpy(read->Query, select.data(), select.size());
    auto result = sql.Read(read, 1).Cast<TCommandResponse>();
    std::string text(result->Data, result->Len - sizeof(TCommandResponse));
    ASSERT_TRUE(text.find("ok") != std::string::npos);
}
TEST(SqlPayloads, sql_validation_runs_before_request_admission) {
    auto state = std::make_shared<TState>();
    auto raft = std::make_shared<TRaft>(state, 1, TNodeDict{});
    auto sql = std::make_shared<TSql>(":memory:", 1);
    TRequestProcessor processor(raft, sql, {});
    for (bool write : {false, true}) {
        raft->Become(write ? EState::FOLLOWER : EState::LEADER);
        auto command = NewHoldedMessage<TWriteSql>();
        command->Flags = write ? TCommandRequest::EWrite : TCommandRequest::EConsistent;
        command->QuerySize = UINT32_MAX;
        Reject([&] { processor.OnCommandRequest(command, {}); });
        ASSERT_EQ(state->LastLogIndex, 0);
    }
}
TEST(SqlPayloads, sql_read_cannot_insert_update_or_delete) {
    for (const std::string query : {"INSERT INTO t VALUES ('bad')", "UPDATE t SET value='bad'", "DELETE FROM t"}) {
        TSql sql(":memory:", 1);
        Seed(sql);
        Reject([&] { sql.Read(Query(query), 1); });
        ASSERT_TRUE(ReadText(sql, "SELECT value FROM t").find("original") != std::string::npos);
        ASSERT_TRUE(ReadText(sql, "SELECT count(*) FROM t").find("1") != std::string::npos);
        ASSERT_EQ(sql.LastAppliedIndex, 1);
    }
}
TEST(SqlPayloads, sql_read_rejects_multi_statement_mutation_before_execution) {
    TSql sql(":memory:", 1);
    Seed(sql);
    Reject([&] { sql.Read(Query("SELECT value FROM t; UPDATE t SET value='bad';"), 1); });
    ASSERT_TRUE(ReadText(sql, "SELECT value FROM t").find("original") != std::string::npos);
}
TEST(SqlPayloads, sql_read_cannot_change_schema) {
    for (const std::string query : {"DROP TABLE t", "CREATE TABLE rogue (x)", "ALTER TABLE t ADD COLUMN x"}) {
        TSql sql(":memory:", 1);
        Seed(sql);
        Reject([&] { sql.Read(Query(query), 1); });
        ASSERT_TRUE(ReadText(sql, "SELECT value FROM t").find("original") != std::string::npos);
    }
}
TEST(SqlPayloads, sql_read_rejects_connection_control_and_unsafe_functions) {
    for (const std::string query : {"PRAGMA query_only=OFF", "BEGIN", "COMMIT", "SAVEPOINT x",
         "ATTACH ':memory:' AS other", "VACUUM", "SELECT load_extension('nonexistent')",
         "SELECT readfile('nonexistent')"}) {
        TSql sql(":memory:", 1);
        Reject([&] { sql.Read(Query(query), 0); });
        ASSERT_EQ(sql.LastAppliedIndex, 0);
    }
}
TEST(SqlPayloads, sql_read_supports_comments_ctes_and_safe_functions) {
    TSql sql(":memory:", 1);
    Seed(sql);
    ASSERT_TRUE(ReadText(sql, "-- leading comment\n SELECT upper(value), 'a;b' FROM t; -- tail").find("ORIGINAL") != std::string::npos);
    ASSERT_TRUE(ReadText(sql, "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<3) SELECT sum(x) FROM n").find("6") != std::string::npos);
}
TEST(SqlPayloads, sql_read_rejects_nul_empty_and_second_select) {
    TSql sql(":memory:", 1);
    const std::string embedded("SELECT 1\0;SELECT 2", 18);
    for (const auto& query : {std::string{}, std::string("-- only comment"), std::string("SELECT 1; SELECT 2"), embedded})
        Reject([&] { sql.Read(Query(query), 0); });
}
TEST(SqlPayloads, sql_read_policy_is_restored_after_rejection) {
    TSql sql(":memory:", 1);
    Seed(sql);
    Reject([&] { sql.Read(Query("DELETE FROM t"), 1); });
    Apply(sql, "INSERT INTO t VALUES ('second')", 2);
    ASSERT_EQ(sql.LastAppliedIndex, 2);
    ASSERT_TRUE(ReadText(sql, "SELECT count(*) FROM t").find("2") != std::string::npos);
}
struct TDatabase {
    std::filesystem::path Directory;
    std::string Path;
    TDatabase() {
        static unsigned counter = 0;
        Directory = std::filesystem::temp_directory_path() /
            ("miniraft-sql-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(counter++));
        EXPECT_TRUE(std::filesystem::create_directory(Directory));
        if (!std::filesystem::is_directory(Directory)) {
            throw std::runtime_error("cannot create " + Directory.string());
        }
        Path = (Directory / "db").string();
    }
    ~TDatabase() { std::error_code ignored; std::filesystem::remove_all(Directory, ignored); }
    void External(const std::string& query) {
        sqlite3* db = nullptr;
        ASSERT_EQ(sqlite3_open((Path + ".1").c_str(), &db), SQLITE_OK);
        int rc = sqlite3_exec(db, query.c_str(), nullptr, nullptr, nullptr);
        sqlite3_close(db);
        ASSERT_EQ(rc, SQLITE_OK);
    }
};
TEST(SqlPayloads, sql_write_cannot_escape_managed_transaction) {
    for (const std::string control : {"COMMIT", "END", "ROLLBACK", "BEGIN", "SAVEPOINT x", "RELEASE x",
         "PRAGMA user_version=7", "ATTACH ':memory:' AS other"}) {
        TSql sql(":memory:", 1);
        Seed(sql);
        auto error = Apply(sql, "INSERT INTO t VALUES ('partial'); " + control + "; INSERT INTO missing VALUES (1);", 2);
        ASSERT_FALSE(error.empty());
        ASSERT_TRUE(ReadText(sql, "SELECT count(*) FROM t").find("\n1\n") != std::string::npos);
        ASSERT_EQ(sql.LastAppliedIndex, 2);
    }
}
TEST(SqlPayloads, sql_write_cannot_modify_progress_metadata) {
    for (const std::string mutation : {"DELETE FROM raft_metadata_", "UPDATE RAFT_METADATA_ SET value='999'",
         "DROP TABLE raft_metadata_", "ALTER TABLE raft_metadata_ RENAME TO stolen",
         "CREATE INDEX tamper ON raft_metadata_ (value)",
         "CREATE TRIGGER tamper AFTER UPDATE ON raft_metadata_ BEGIN SELECT 1; END",
         "CREATE TRIGGER steal AFTER INSERT ON t BEGIN UPDATE raft_metadata_ SET value='999'; END; INSERT INTO t VALUES ('bad')"}) {
        TSql sql(":memory:", 1);
        Seed(sql);
        auto error = Apply(sql, "INSERT INTO t VALUES ('partial'); " + mutation, 2);
        ASSERT_FALSE(error.empty());
        ASSERT_TRUE(ReadText(sql, "SELECT count(*) FROM t").find("\n1\n") != std::string::npos);
        ASSERT_TRUE(ReadText(sql, "SELECT value FROM raft_metadata_ WHERE key='LastAppliedIndex'").find("\n2\n") != std::string::npos);
        ASSERT_EQ(sql.LastAppliedIndex, 2);
    }
}
TEST(SqlPayloads, sql_failed_write_progress_survives_restart) {
    TDatabase file;
    {
        TSql sql(file.Path, 1);
        Seed(sql);
        ASSERT_FALSE(Apply(sql, "INSERT INTO t VALUES ('partial'); INSERT INTO missing VALUES (1)", 2).empty());
        ASSERT_EQ(sql.LastAppliedIndex, 2);
        ASSERT_TRUE(ReadText(sql, "SELECT count(*) FROM t").find("\n1\n") != std::string::npos);
    }
    {
        TSql sql(file.Path, 1);
        ASSERT_EQ(sql.LastAppliedIndex, 2);
        Apply(sql, "INSERT INTO t VALUES ('must not replay')", 2);
        ASSERT_TRUE(ReadText(sql, "SELECT count(*) FROM t").find("\n1\n") != std::string::npos);
        ASSERT_TRUE(Apply(sql, "INSERT INTO t VALUES ('next')", 3).empty());
    }
    TSql sql(file.Path, 1);
    ASSERT_EQ(sql.LastAppliedIndex, 3);
    ASSERT_TRUE(ReadText(sql, "SELECT count(*) FROM t").find("\n2\n") != std::string::npos);
}
TEST(SqlPayloads, sql_write_handles_trailing_comment_without_losing_progress) {
    TDatabase file;
    {
        TSql sql(file.Path, 1);
        Seed(sql);
        ASSERT_TRUE(Apply(sql, "INSERT INTO t VALUES ('next'); -- trailing comment", 2).empty());
        ASSERT_EQ(sql.LastAppliedIndex, 2);
    }
    TSql sql(file.Path, 1);
    ASSERT_EQ(sql.LastAppliedIndex, 2);
    ASSERT_TRUE(ReadText(sql, "SELECT count(*) FROM t").find("\n2\n") != std::string::npos);
}
TEST(SqlPayloads, sql_automatic_rollback_is_consumed_without_partial_data) {
    TSql sql(":memory:", 1);
    ASSERT_TRUE(Apply(sql, "CREATE TABLE u (x INTEGER UNIQUE); INSERT INTO u VALUES (1)", 1).empty());
    ASSERT_FALSE(Apply(sql, "INSERT INTO u VALUES (2); INSERT OR ROLLBACK INTO u VALUES (1)", 2).empty());
    ASSERT_EQ(sql.LastAppliedIndex, 2);
    ASSERT_TRUE(ReadText(sql, "SELECT count(*) FROM u").find("\n1\n") != std::string::npos);
    ASSERT_TRUE(Apply(sql, "INSERT INTO u VALUES (3)", 3).empty());
}
TEST(SqlPayloads, sql_metadata_failure_rolls_back_data_and_does_not_advance) {
    TDatabase file;
    { TSql sql(file.Path, 1); Seed(sql); }
    file.External("CREATE TRIGGER fail_marker BEFORE UPDATE ON raft_metadata_ BEGIN SELECT RAISE(ABORT,'marker blocked'); END");
    {
        TSql sql(file.Path, 1);
        bool failed = false;
        try { Apply(sql, "INSERT INTO t VALUES ('partial')", 2); }
        catch (const std::runtime_error&) { failed = true; }
        ASSERT_TRUE(failed);
        ASSERT_EQ(sql.LastAppliedIndex, 1);
        ASSERT_TRUE(ReadText(sql, "SELECT count(*) FROM t").find("\n1\n") != std::string::npos);
    }
    file.External("DROP TRIGGER fail_marker");
    TSql sql(file.Path, 1);
    ASSERT_EQ(sql.LastAppliedIndex, 1);
    ASSERT_TRUE(Apply(sql, "INSERT INTO t VALUES ('retry')", 2).empty());
}
TEST(SqlPayloads, sql_progress_restores_full_uint64_range) {
    TDatabase file;
    { TSql sql(file.Path, 1); ASSERT_TRUE(Apply(sql, "CREATE TABLE t (x)", UINT64_MAX).empty()); }
    TSql sql(file.Path, 1);
    ASSERT_TRUE(sql.LastAppliedIndex == UINT64_MAX);
}
TEST(SqlPayloads, sql_invalid_progress_fails_closed) {
    for (const std::string value : {"NULL", "'-1'", "'42junk'", "'18446744073709551616'", "''", "CAST(X'3432006a756e6b' AS TEXT)"}) {
        TDatabase file;
        { TSql sql(file.Path, 1); Seed(sql); }
        file.External("UPDATE raft_metadata_ SET value=" + value);
        bool failed = false;
        try { TSql sql(file.Path, 1); }
        catch (const std::exception&) { failed = true; }
        ASSERT_TRUE(failed);
    }
}

TEST(SqlPayloads, sql_busy_write_does_not_consume_entry) {
    for (const std::string lock : {"BEGIN IMMEDIATE", "BEGIN; SELECT * FROM t"}) {
        TDatabase file;
        TSql sql(file.Path, 1);
        Seed(sql);
        sqlite3* other = nullptr;
        ASSERT_EQ(sqlite3_open((file.Path + ".1").c_str(), &other), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(other, lock.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
        bool failed = false;
        try { Apply(sql, "INSERT INTO t VALUES ('busy')", 2); }
        catch (const std::runtime_error&) { failed = true; }
        sqlite3_exec(other, "ROLLBACK", nullptr, nullptr, nullptr);
        sqlite3_close(other);
        ASSERT_TRUE(failed);
        ASSERT_EQ(sql.LastAppliedIndex, 1);
        ASSERT_TRUE(ReadText(sql, "SELECT count(*) FROM t").find("\n1\n") != std::string::npos);
        ASSERT_TRUE(Apply(sql, "INSERT INTO t VALUES ('retry')", 2).empty());
        ASSERT_EQ(sql.LastAppliedIndex, 2);
    }
}
TEST(SqlPayloads, sql_write_error_classification) {
    // SQLITE_TOOBIG at the default 10^9-byte limit needs about 1 GB, and TSql keeps its
    // connection private, so the classification is checked directly.
    for (int code : {SQLITE_ERROR, SQLITE_AUTH, SQLITE_CONSTRAINT, SQLITE_CONSTRAINT_UNIQUE,
                     SQLITE_MISMATCH, SQLITE_TOOBIG, SQLITE_RANGE}) {
        ASSERT_TRUE(IsDeterministicSqlError(code));
    }
    for (int code : {SQLITE_BUSY, SQLITE_LOCKED, SQLITE_IOERR, SQLITE_IOERR_WRITE, SQLITE_NOMEM,
                     SQLITE_FULL, SQLITE_INTERRUPT, SQLITE_READONLY, SQLITE_CORRUPT}) {
        ASSERT_FALSE(IsDeterministicSqlError(code));
    }
}
TEST(SqlPayloads, sql_write_rejects_embedded_nul_before_admission) {
    TSql sql(":memory:", 1);
    const std::string text = std::string("CREATE TABLE t (x);") + '\0' + "COMMIT";
    auto command = Query(text);
    command->Flags = TCommandRequest::EWrite;
    Reject([&] { sql.Validate(command); });
    Reject([&] { sql.Prepare(command); });
    auto entry = NewHoldedMessage<TSqlLogEntry>(sizeof(TSqlLogEntry) + text.size());
    entry->QuerySize = text.size();
    memcpy(entry->Query, text.data(), text.size());
    Reject([&] { sql.Write(entry, 1); });
    ASSERT_EQ(sql.LastAppliedIndex, 0);
}

const std::string ExpensiveRead = "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<100000) SELECT sum(x) FROM n";
TEST(SqlPayloads, sql_read_stops_at_work_budget) {
    TSql sql(":memory:", 1, {.MaxVmSteps = 1000});
    Reject([&] { sql.Read(Query(ExpensiveRead), 0); });
    ASSERT_TRUE(ReadText(sql, "SELECT 7 AS v") == "v\n7\n");
    ASSERT_EQ(sql.LastAppliedIndex, 0);
    TSql defaults(":memory:", 1);
    Reject([&] { defaults.Read(Query(ExpensiveRead), 0); });
}
TEST(SqlPayloads, sql_read_output_budget_counts_headers_separators_and_null) {
    for (const auto& query : {std::string("SELECT 'abc' AS v"), std::string("SELECT NULL AS v"),
         std::string("SELECT 1 AS a, 2 AS b"), std::string("SELECT 1 AS v WHERE 0")}) {
        TSql reference(":memory:", 1);
        const auto expected = ReadText(reference, query);
        TSql exact(":memory:", 1, {.MaxResultBytes = expected.size()});
        ASSERT_TRUE(ReadText(exact, query) == expected);
        TSql shortByOne(":memory:", 1, {.MaxResultBytes = expected.size() - 1});
        Reject([&] { shortByOne.Read(Query(query), 0); });
    }
}
TEST(SqlPayloads, sql_read_bounds_accumulated_rows_and_cleans_up) {
    TSql sql(":memory:", 1, {.MaxResultBytes = 64});
    Reject([&] { sql.Read(Query("WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<10000) SELECT x FROM n"), 0); });
    ASSERT_TRUE(ReadText(sql, "SELECT 8 AS v") == "v\n8\n");
    ASSERT_TRUE(Apply(sql, "CREATE TABLE t (x); INSERT INTO t VALUES (9)", 1).empty());
    ASSERT_TRUE(ReadText(sql, "SELECT x FROM t") == "x\n9\n");
}
TEST(SqlPayloads, sql_read_budget_failures_recover_through_request_processor) {
    struct TSink : INode {
        std::vector<std::pair<uint32_t, uint32_t>> Replies;
        void Send(TMessageHolder<TMessage> message) override {
            auto reply = message.Cast<TCommandResponse>();
            Replies.emplace_back(reply->Cookie, reply->ErrorCode);
        }
        void Drain() override {}
    };
    auto raft = std::make_shared<TRaft>(std::make_shared<TState>(), 1, TNodeDict{});
    raft->ProcessTimeout(ITimeSource::Time{});
    auto sql = std::make_shared<TSql>(":memory:", 1, TSqlReadLimits{.MaxVmSteps = 1000, .MaxResultBytes = 64});
    TRequestProcessor processor(raft, sql, {});
    auto client = std::make_shared<TSink>();
    uint32_t cookie = 1;
    for (uint32_t flags : {0u, uint32_t(TCommandRequest::EConsistent)}) {
        for (const auto& query : {ExpensiveRead, std::string("SELECT 'abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz' AS v"), std::string("SELECT 1 AS v")}) {
            auto command = Query(query);
            command->Flags = flags;
            command->Cookie = cookie++;
            processor.OnCommandRequest(command, client);
            processor.ProcessCommitted();
            processor.ProcessWaiting();
        }
    }
    ASSERT_EQ(client->Replies.size(), 6);
    for (size_t i = 0; i < client->Replies.size(); ++i) {
        ASSERT_EQ(client->Replies[i].first, i + 1);
        ASSERT_EQ(client->Replies[i].second, i % 3 == 2 ? 0 : 2);
    }
    ASSERT_EQ(raft->CurrentStateName(), EState::LEADER);
}
TEST(SqlPayloads, sql_read_preserves_nul_bytes_and_counts_their_length) {
    TSql sql(":memory:", 1, {.MaxResultBytes = 6});
    ASSERT_TRUE(ReadText(sql, "SELECT CAST(X'610062' AS TEXT) AS v") == std::string("v\na\0b\n", 6));
    Reject([&] { sql.Read(Query("SELECT CAST(X'61006263' AS TEXT) AS v"), 0); });
}
TEST(SqlPayloads, sql_read_rejects_invalid_limits) {
    for (const auto limits : {TSqlReadLimits{.MaxVmSteps = 0},
         TSqlReadLimits{.MaxResultBytes = TMessageLimits{}.MaxFrameBytes}}) {
        bool rejected = false;
        try { TSql sql(":memory:", 1, limits); }
        catch (const std::invalid_argument&) { rejected = true; }
        ASSERT_TRUE(rejected);
    }
}

TEST(SqlPayloads, sql_read_value_limit_is_restored_before_writes) {
    TSql sql(":memory:", 1);
    const std::string value(9 * 1024 * 1024, 'x');
    ASSERT_TRUE(Apply(sql, "CREATE TABLE big (v TEXT); INSERT INTO big VALUES ('" + value + "')", 1).empty());
    Reject([&] { sql.Read(Query("SELECT hex(v) FROM big"), 1); });
    // Writes retain their previous SQLite length limit, even after a failed read.
    ASSERT_TRUE(Apply(sql, "UPDATE big SET v=v||v", 2).empty());
    ASSERT_TRUE(Apply(sql, "UPDATE big SET v=substr(v,1,1)", 3).empty());
    ASSERT_TRUE(ReadText(sql, "SELECT v FROM big") == "v\nx\n");
    ASSERT_EQ(sql.LastAppliedIndex, 3);
}

TEST(SqlPayloads, sql_and_raft_restart_do_not_reapply_committed_insert) {
    TDatabase file;
    const auto logPath = file.Path + "-raft";
    {
        auto state = std::make_shared<TDiskState>(logPath, 1);
        auto raft = std::make_shared<TRaft>(state, 1, TNodeDict{});
        auto sql = std::make_shared<TSql>(file.Path, 1);
        TRequestProcessor processor(raft, sql, {});
        auto now = ITimeSource::Time{};
        raft->ProcessTimeout(now);
        for (const auto query : {"CREATE TABLE t (value TEXT)", "INSERT INTO t VALUES ('once')"}) {
            auto command = Query(query);
            command->Flags = TCommandRequest::EWrite;
            processor.OnCommandRequest(command, {});
            now += std::chrono::milliseconds(500);
            raft->ProcessTimeout(now);
            processor.ProcessCommitted();
        }
        ASSERT_EQ(sql->LastAppliedIndex, 3);
        ASSERT_TRUE(ReadText(*sql, "SELECT count(*) AS n FROM t") == "n\n1\n");
    }
    {
        auto state = std::make_shared<TDiskState>(logPath, 1);
        auto raft = std::make_shared<TRaft>(state, 1, TNodeDict{});
        auto sql = std::make_shared<TSql>(file.Path, 1);
        ASSERT_EQ(sql->LastAppliedIndex, 3);
        TRequestProcessor processor(raft, sql, {});
        processor.ProcessCommitted(); // server timer can fire before any leader is known
        const auto beforeElection = sql->LastAppliedIndex;
        raft->ProcessTimeout(ITimeSource::Time{});
        processor.ProcessCommitted();
        processor.ProcessCommitted();
        ASSERT_TRUE(ReadText(*sql, "SELECT count(*) AS n FROM t") == "n\n1\n");
        ASSERT_EQ(beforeElection, 3);
        ASSERT_EQ(sql->LastAppliedIndex, 4); // new-term stub advanced framework progress
    }
}

// Larger than one snapshot record; written without the application's policy.
void FillBig(TDatabase& file) {
    file.External("CREATE TABLE big (v BLOB); WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c "
                  "WHERE x < 3000) INSERT INTO big SELECT randomblob(1000) FROM c;");
}
TEST(SqlPayloads, sql_snapshot_restores_database_and_progress) {
    TDatabase source;
    FillBig(source);
    TSql leader(source.Path, 1);
    Seed(leader);
    Apply(leader, "INSERT INTO t VALUES ('second')", 2);
    auto snapshot = leader.Snapshot();
    ASSERT_EQ(snapshot->LastIncludedIndex, 2);
    ASSERT_TRUE(snapshot.PayloadSize > 1);
    TDatabase target;
    {
        TSql follower(target.Path, 1);
        Apply(follower, "CREATE TABLE stale (x)", 1);
        follower.Apply(snapshot);
        ASSERT_EQ(follower.LastAppliedIndex, 2);
        ASSERT_TRUE(ReadText(follower, "SELECT value FROM t") == "value\noriginal\nsecond\n");
        ASSERT_TRUE(ReadText(follower, "SELECT count(*) AS n FROM big") == "n\n3000\n");
        Reject([&] { ReadText(follower, "SELECT x FROM stale"); });
        Apply(follower, "INSERT INTO t VALUES ('third')", 3);
    }
    TSql reopened(target.Path, 1);
    ASSERT_EQ(reopened.LastAppliedIndex, 3);
    ASSERT_TRUE(ReadText(reopened, "SELECT value FROM t") == "value\noriginal\nsecond\nthird\n");
}
TEST(SqlPayloads, sql_invalid_snapshot_leaves_database_unchanged) {
    TDatabase source;
    FillBig(source);
    TSql leader(source.Path, 1);
    Seed(leader);
    TSql follower(":memory:", 1);
    Seed(follower);
    Apply(follower, "INSERT INTO t VALUES ('mine')", 2);
    const auto before = ReadText(follower, "SELECT value FROM t");
    auto check = [&](TMessageHolder<TInstallSnapshotRequest> image) {
        bool rejected = false;
        try { follower.Apply(image); } catch (const std::exception&) { rejected = true; }
        ASSERT_TRUE(rejected);
        ASSERT_EQ(follower.LastAppliedIndex, 2);
        ASSERT_TRUE(ReadText(follower, "SELECT value FROM t") == before);
    };
    auto moved = leader.Snapshot();
    moved->LastIncludedIndex = 7;
    check(moved);
    auto truncated = leader.Snapshot();
    truncated.PayloadSize -= 1;
    check(truncated);
    auto garbage = NewHoldedMessage(TMessageEx{}, TInstallSnapshotRequest{.LastIncludedIndex = 1});
    const std::string text = "not a database";
    auto record = NewHoldedMessage<TLogEntry>(sizeof(TLogEntry) + text.size());
    memcpy(record->Data, text.data(), text.size());
    garbage.InitPayload(1);
    garbage.Payload[0] = record;
    check(garbage);
}
TEST(SqlPayloads, sql_snapshot_after_an_election_stub_installs_and_restarts) {
    TDatabase file;
    const auto logPath = file.Path + "-raft";
    {
        auto state = std::make_shared<TDiskState>(logPath, 1);
        auto raft = std::make_shared<TRaft>(state, 1, TNodeDict{});
        auto sql = std::make_shared<TSql>(file.Path, 1);
        TRequestProcessor processor(raft, sql, {});
        auto now = ITimeSource::Time{};
        raft->ProcessTimeout(now);
        for (const auto query : {"CREATE TABLE t (value TEXT)", "INSERT INTO t VALUES ('once')"}) {
            auto command = Query(query);
            command->Flags = TCommandRequest::EWrite;
            processor.OnCommandRequest(command, {});
            now += std::chrono::milliseconds(500);
            raft->ProcessTimeout(now);
            processor.ProcessCommitted();
        }
        ASSERT_EQ(sql->LastAppliedIndex, 3);
    }
    TMessageHolder<TInstallSnapshotRequest> image;
    {
        // After a restart and an election the stub at 4 is applied without a write.
        auto state = std::make_shared<TDiskState>(logPath, 1);
        auto raft = std::make_shared<TRaft>(state, 1, TNodeDict{});
        auto sql = std::make_shared<TSql>(file.Path, 1);
        TRequestProcessor processor(raft, sql, {});
        raft->ProcessTimeout(ITimeSource::Time{});
        processor.ProcessCommitted();
        ASSERT_EQ(sql->LastAppliedIndex, 4);
        raft->Compact(sql->Snapshot());
        ASSERT_EQ(state->SnapshotIndex, 3);
        image = state->GetSnapshot();
    }
    {
        // A lagging follower installs the image.
        auto state = std::make_shared<TState>();
        state->InstallSnapshot(image);
        auto follower = std::make_shared<TSql>(":memory:", 2);
        TRequestProcessor processor(std::make_shared<TRaft>(state, 2, TNodeDict{}), follower, {});
        processor.ProcessCommitted();
        ASSERT_EQ(follower->LastAppliedIndex, 3);
        ASSERT_TRUE(ReadText(*follower, "SELECT value FROM t") == "value\nonce\n");
    }
    // The compacted node restarts before its next write.
    auto state = std::make_shared<TDiskState>(logPath, 1);
    auto raft = std::make_shared<TRaft>(state, 1, TNodeDict{});
    auto sql = std::make_shared<TSql>(file.Path, 1);
    TRequestProcessor processor(raft, sql, {});
    processor.ProcessCommitted();
    ASSERT_EQ(sql->LastAppliedIndex, 3);
    ASSERT_TRUE(ReadText(*sql, "SELECT value FROM t") == "value\nonce\n");
}
TEST(SqlPayloads, sql_restores_from_a_stored_snapshot) {
    TSql source(":memory:", 1);
    Seed(source);
    auto image = source.Snapshot();
    image->LastIncludedTerm = 1;
    auto state = std::make_shared<TState>();
    state->InstallSnapshot(image);
    auto raft = std::make_shared<TRaft>(state, 1, TNodeDict{});
    TDatabase file;
    auto sql = std::make_shared<TSql>(file.Path, 1);
    TRequestProcessor processor(raft, sql, {});
    processor.ProcessCommitted();
    ASSERT_EQ(sql->LastAppliedIndex, 1);
    ASSERT_TRUE(ReadText(*sql, "SELECT value FROM t") == "value\noriginal\n");
}

}
