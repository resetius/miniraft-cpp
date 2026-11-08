#include "../examples/sql_rsm.h"
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
void Apply(TSql& sql, const std::string& query, uint64_t index) {
    auto command = Query(query);
    command->Flags = TCommandRequest::EWrite;
    sql.Write(sql.Prepare(command), index);
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
    };
    return cmocka_run_group_tests(tests, nullptr, nullptr);
}
