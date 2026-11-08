#include "../examples/sql_rsm.h"
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
extern "C" {
#include <cmocka.h>
}
namespace {
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
}
int main() {
    const CMUnitTest tests[] = {
        cmocka_unit_test(test_sql_prepare_rejects_short_query),
        cmocka_unit_test(test_sql_read_rejects_short_query),
        cmocka_unit_test(test_sql_apply_rejects_before_progress),
        cmocka_unit_test(test_valid_sql_payload_roundtrip),
        cmocka_unit_test(test_sql_validation_runs_before_request_admission),
    };
    return cmocka_run_group_tests(tests, nullptr, nullptr);
}
