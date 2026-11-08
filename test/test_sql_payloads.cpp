#include "../examples/sql_rsm.h"
#include <filesystem>
#include <chrono>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
extern "C" {
#include <cmocka.h>
}
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
    assert_true(rejected);
}
void test_sql_prepare_rejects_short_query(void**) {
    TSql sql(":memory:", 1);
    auto command = NewHoldedMessage<TWriteSql>();
    command->QuerySize = UINT32_MAX;
    Reject([&] { sql.Prepare(command); });
}
void test_sql_read_rejects_short_query(void**) {
    TSql sql(":memory:", 1);
    auto command = NewHoldedMessage<TReadSql>();
    command->QuerySize = UINT32_MAX;
    Reject([&] { sql.Read(command, 0); });
}
void test_sql_apply_rejects_before_progress(void**) {
    TSql sql(":memory:", 1);
    auto entry = NewHoldedMessage<TSqlLogEntry>();
    entry->QuerySize = UINT32_MAX;
    Reject([&] { sql.Write(entry, 1); });
    assert_int_equal(sql.LastAppliedIndex, 0);
}
void test_valid_sql_payload_roundtrip(void**) {
    TSql sql(":memory:", 1);
    const std::string query = "CREATE TABLE test (value TEXT); INSERT INTO test VALUES ('ok');";
    auto command = NewHoldedMessage<TWriteSql>(sizeof(TWriteSql) + query.size());
    command->QuerySize = query.size();
    memcpy(command->Query, query.data(), query.size());
    sql.Write(sql.Prepare(command), 1);
    assert_int_equal(sql.LastAppliedIndex, 1);
    const std::string select = "SELECT value FROM test";
    auto read = NewHoldedMessage<TReadSql>(sizeof(TReadSql) + select.size());
    read->QuerySize = select.size();
    memcpy(read->Query, select.data(), select.size());
    auto result = sql.Read(read, 1).Cast<TCommandResponse>();
    std::string text(result->Data, result->Len - sizeof(TCommandResponse));
    assert_true(text.find("ok") != std::string::npos);
}
void test_sql_validation_runs_before_request_admission(void**) {
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
        assert_int_equal(state->LastLogIndex, 0);
    }
}
void test_sql_read_cannot_insert_update_or_delete(void**) {
    for (const std::string query : {"INSERT INTO t VALUES ('bad')", "UPDATE t SET value='bad'", "DELETE FROM t"}) {
        TSql sql(":memory:", 1);
        Seed(sql);
        Reject([&] { sql.Read(Query(query), 1); });
        assert_true(ReadText(sql, "SELECT value FROM t").find("original") != std::string::npos);
        assert_true(ReadText(sql, "SELECT count(*) FROM t").find("1") != std::string::npos);
        assert_int_equal(sql.LastAppliedIndex, 1);
    }
}
void test_sql_read_rejects_multi_statement_mutation_before_execution(void**) {
    TSql sql(":memory:", 1);
    Seed(sql);
    Reject([&] { sql.Read(Query("SELECT value FROM t; UPDATE t SET value='bad';"), 1); });
    assert_true(ReadText(sql, "SELECT value FROM t").find("original") != std::string::npos);
}
void test_sql_read_cannot_change_schema(void**) {
    for (const std::string query : {"DROP TABLE t", "CREATE TABLE rogue (x)", "ALTER TABLE t ADD COLUMN x"}) {
        TSql sql(":memory:", 1);
        Seed(sql);
        Reject([&] { sql.Read(Query(query), 1); });
        assert_true(ReadText(sql, "SELECT value FROM t").find("original") != std::string::npos);
    }
}
void test_sql_read_rejects_connection_control_and_unsafe_functions(void**) {
    for (const std::string query : {"PRAGMA query_only=OFF", "BEGIN", "COMMIT", "SAVEPOINT x",
         "ATTACH ':memory:' AS other", "VACUUM", "SELECT load_extension('nonexistent')",
         "SELECT readfile('nonexistent')"}) {
        TSql sql(":memory:", 1);
        Reject([&] { sql.Read(Query(query), 0); });
        assert_int_equal(sql.LastAppliedIndex, 0);
    }
}
void test_sql_read_supports_comments_ctes_and_safe_functions(void**) {
    TSql sql(":memory:", 1);
    Seed(sql);
    assert_true(ReadText(sql, "-- leading comment\n SELECT upper(value), 'a;b' FROM t; -- tail").find("ORIGINAL") != std::string::npos);
    assert_true(ReadText(sql, "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<3) SELECT sum(x) FROM n").find("6") != std::string::npos);
}
void test_sql_read_rejects_nul_empty_and_second_select(void**) {
    TSql sql(":memory:", 1);
    const std::string embedded("SELECT 1\0;SELECT 2", 18);
    for (const auto& query : {std::string{}, std::string("-- only comment"), std::string("SELECT 1; SELECT 2"), embedded})
        Reject([&] { sql.Read(Query(query), 0); });
}
void test_sql_read_policy_is_restored_after_rejection(void**) {
    TSql sql(":memory:", 1);
    Seed(sql);
    Reject([&] { sql.Read(Query("DELETE FROM t"), 1); });
    Apply(sql, "INSERT INTO t VALUES ('second')", 2);
    assert_int_equal(sql.LastAppliedIndex, 2);
    assert_true(ReadText(sql, "SELECT count(*) FROM t").find("2") != std::string::npos);
}
struct TDatabase {
    std::filesystem::path Directory;
    std::string Path;
    TDatabase() {
        static unsigned counter = 0;
        Directory = std::filesystem::temp_directory_path() /
            ("miniraft-sql-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(counter++));
        assert_true(std::filesystem::create_directory(Directory));
        Path = (Directory / "db").string();
    }
    ~TDatabase() { std::error_code ignored; std::filesystem::remove_all(Directory, ignored); }
    void External(const std::string& query) {
        sqlite3* db = nullptr;
        assert_int_equal(sqlite3_open((Path + ".1").c_str(), &db), SQLITE_OK);
        int rc = sqlite3_exec(db, query.c_str(), nullptr, nullptr, nullptr);
        sqlite3_close(db);
        assert_int_equal(rc, SQLITE_OK);
    }
};
void test_sql_write_cannot_escape_managed_transaction(void**) {
    for (const std::string control : {"COMMIT", "END", "ROLLBACK", "BEGIN", "SAVEPOINT x", "RELEASE x",
         "PRAGMA user_version=7", "ATTACH ':memory:' AS other"}) {
        TSql sql(":memory:", 1);
        Seed(sql);
        auto error = Apply(sql, "INSERT INTO t VALUES ('partial'); " + control + "; INSERT INTO missing VALUES (1);", 2);
        assert_false(error.empty());
        assert_true(ReadText(sql, "SELECT count(*) FROM t").find("\n1\n") != std::string::npos);
        assert_int_equal(sql.LastAppliedIndex, 2);
    }
}
void test_sql_write_cannot_modify_progress_metadata(void**) {
    for (const std::string mutation : {"DELETE FROM raft_metadata_", "UPDATE RAFT_METADATA_ SET value='999'",
         "DROP TABLE raft_metadata_", "ALTER TABLE raft_metadata_ RENAME TO stolen",
         "CREATE INDEX tamper ON raft_metadata_ (value)",
         "CREATE TRIGGER tamper AFTER UPDATE ON raft_metadata_ BEGIN SELECT 1; END",
         "CREATE TRIGGER steal AFTER INSERT ON t BEGIN UPDATE raft_metadata_ SET value='999'; END; INSERT INTO t VALUES ('bad')"}) {
        TSql sql(":memory:", 1);
        Seed(sql);
        auto error = Apply(sql, "INSERT INTO t VALUES ('partial'); " + mutation, 2);
        assert_false(error.empty());
        assert_true(ReadText(sql, "SELECT count(*) FROM t").find("\n1\n") != std::string::npos);
        assert_true(ReadText(sql, "SELECT value FROM raft_metadata_ WHERE key='LastAppliedIndex'").find("\n2\n") != std::string::npos);
        assert_int_equal(sql.LastAppliedIndex, 2);
    }
}
void test_sql_failed_write_progress_survives_restart(void**) {
    TDatabase file;
    {
        TSql sql(file.Path, 1);
        Seed(sql);
        assert_false(Apply(sql, "INSERT INTO t VALUES ('partial'); INSERT INTO missing VALUES (1)", 2).empty());
        assert_int_equal(sql.LastAppliedIndex, 2);
        assert_true(ReadText(sql, "SELECT count(*) FROM t").find("\n1\n") != std::string::npos);
    }
    {
        TSql sql(file.Path, 1);
        assert_int_equal(sql.LastAppliedIndex, 2);
        Apply(sql, "INSERT INTO t VALUES ('must not replay')", 2);
        assert_true(ReadText(sql, "SELECT count(*) FROM t").find("\n1\n") != std::string::npos);
        assert_true(Apply(sql, "INSERT INTO t VALUES ('next')", 3).empty());
    }
    TSql sql(file.Path, 1);
    assert_int_equal(sql.LastAppliedIndex, 3);
    assert_true(ReadText(sql, "SELECT count(*) FROM t").find("\n2\n") != std::string::npos);
}
void test_sql_write_handles_trailing_comment_without_losing_progress(void**) {
    TDatabase file;
    {
        TSql sql(file.Path, 1);
        Seed(sql);
        assert_true(Apply(sql, "INSERT INTO t VALUES ('next'); -- trailing comment", 2).empty());
        assert_int_equal(sql.LastAppliedIndex, 2);
    }
    TSql sql(file.Path, 1);
    assert_int_equal(sql.LastAppliedIndex, 2);
    assert_true(ReadText(sql, "SELECT count(*) FROM t").find("\n2\n") != std::string::npos);
}
void test_sql_automatic_rollback_is_consumed_without_partial_data(void**) {
    TSql sql(":memory:", 1);
    assert_true(Apply(sql, "CREATE TABLE u (x INTEGER UNIQUE); INSERT INTO u VALUES (1)", 1).empty());
    assert_false(Apply(sql, "INSERT INTO u VALUES (2); INSERT OR ROLLBACK INTO u VALUES (1)", 2).empty());
    assert_int_equal(sql.LastAppliedIndex, 2);
    assert_true(ReadText(sql, "SELECT count(*) FROM u").find("\n1\n") != std::string::npos);
    assert_true(Apply(sql, "INSERT INTO u VALUES (3)", 3).empty());
}
void test_sql_metadata_failure_rolls_back_data_and_does_not_advance(void**) {
    TDatabase file;
    { TSql sql(file.Path, 1); Seed(sql); }
    file.External("CREATE TRIGGER fail_marker BEFORE UPDATE ON raft_metadata_ BEGIN SELECT RAISE(ABORT,'marker blocked'); END");
    {
        TSql sql(file.Path, 1);
        bool failed = false;
        try { Apply(sql, "INSERT INTO t VALUES ('partial')", 2); }
        catch (const std::runtime_error&) { failed = true; }
        assert_true(failed);
        assert_int_equal(sql.LastAppliedIndex, 1);
        assert_true(ReadText(sql, "SELECT count(*) FROM t").find("\n1\n") != std::string::npos);
    }
    file.External("DROP TRIGGER fail_marker");
    TSql sql(file.Path, 1);
    assert_int_equal(sql.LastAppliedIndex, 1);
    assert_true(Apply(sql, "INSERT INTO t VALUES ('retry')", 2).empty());
}
void test_sql_progress_restores_full_uint64_range(void**) {
    TDatabase file;
    { TSql sql(file.Path, 1); assert_true(Apply(sql, "CREATE TABLE t (x)", UINT64_MAX).empty()); }
    TSql sql(file.Path, 1);
    assert_true(sql.LastAppliedIndex == UINT64_MAX);
}
void test_sql_invalid_progress_fails_closed(void**) {
    for (const std::string value : {"NULL", "'-1'", "'42junk'", "'18446744073709551616'", "''", "CAST(X'3432006a756e6b' AS TEXT)"}) {
        TDatabase file;
        { TSql sql(file.Path, 1); Seed(sql); }
        file.External("UPDATE raft_metadata_ SET value=" + value);
        bool failed = false;
        try { TSql sql(file.Path, 1); }
        catch (const std::exception&) { failed = true; }
        assert_true(failed);
    }
}

void test_sql_busy_write_does_not_consume_entry(void**) {
    for (const std::string lock : {"BEGIN IMMEDIATE", "BEGIN; SELECT * FROM t"}) {
        TDatabase file;
        TSql sql(file.Path, 1);
        Seed(sql);
        sqlite3* other = nullptr;
        assert_int_equal(sqlite3_open((file.Path + ".1").c_str(), &other), SQLITE_OK);
        assert_int_equal(sqlite3_exec(other, lock.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
        bool failed = false;
        try { Apply(sql, "INSERT INTO t VALUES ('busy')", 2); }
        catch (const std::runtime_error&) { failed = true; }
        sqlite3_exec(other, "ROLLBACK", nullptr, nullptr, nullptr);
        sqlite3_close(other);
        assert_true(failed);
        assert_int_equal(sql.LastAppliedIndex, 1);
        assert_true(ReadText(sql, "SELECT count(*) FROM t").find("\n1\n") != std::string::npos);
        assert_true(Apply(sql, "INSERT INTO t VALUES ('retry')", 2).empty());
        assert_int_equal(sql.LastAppliedIndex, 2);
    }
}
void test_sql_write_rejects_embedded_nul_before_admission(void**) {
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
    assert_int_equal(sql.LastAppliedIndex, 0);
}

}
int main() {
    const CMUnitTest tests[] = {
        cmocka_unit_test(test_sql_prepare_rejects_short_query),
        cmocka_unit_test(test_sql_read_rejects_short_query),
        cmocka_unit_test(test_sql_apply_rejects_before_progress),
        cmocka_unit_test(test_valid_sql_payload_roundtrip),
        cmocka_unit_test(test_sql_validation_runs_before_request_admission),
        cmocka_unit_test(test_sql_read_cannot_insert_update_or_delete),
        cmocka_unit_test(test_sql_read_rejects_multi_statement_mutation_before_execution),
        cmocka_unit_test(test_sql_read_cannot_change_schema),
        cmocka_unit_test(test_sql_read_rejects_connection_control_and_unsafe_functions),
        cmocka_unit_test(test_sql_read_supports_comments_ctes_and_safe_functions),
        cmocka_unit_test(test_sql_read_rejects_nul_empty_and_second_select),
        cmocka_unit_test(test_sql_read_policy_is_restored_after_rejection),
        cmocka_unit_test(test_sql_write_cannot_escape_managed_transaction),
        cmocka_unit_test(test_sql_write_cannot_modify_progress_metadata),
        cmocka_unit_test(test_sql_failed_write_progress_survives_restart),
        cmocka_unit_test(test_sql_write_handles_trailing_comment_without_losing_progress),
        cmocka_unit_test(test_sql_automatic_rollback_is_consumed_without_partial_data),
        cmocka_unit_test(test_sql_metadata_failure_rolls_back_data_and_does_not_advance),
        cmocka_unit_test(test_sql_progress_restores_full_uint64_range),
        cmocka_unit_test(test_sql_invalid_progress_fails_closed),
        cmocka_unit_test(test_sql_busy_write_does_not_consume_entry),
        cmocka_unit_test(test_sql_write_rejects_embedded_nul_before_admission),
    };
    return cmocka_run_group_tests(tests, nullptr, nullptr);
}
