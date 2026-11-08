#include "../examples/kv.h"
#include "../examples/kv_messages.h"
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif
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
void test_kv_prepare_rejects_inconsistent_lengths(void**) {
    TKv kv;
    auto command = NewHoldedMessage<TWriteKv>();
    command->KeySize = UINT16_MAX;
    command->ValSize = UINT16_MAX;
    Reject([&] { kv.Prepare(command); });
    assert_int_equal(kv.LastAppliedIndex, 0);
}
void test_kv_read_rejects_short_key(void**) {
    TKv kv;
    auto command = NewHoldedMessage<TReadKv>();
    command->KeySize = UINT16_MAX;
    Reject([&] { kv.Read(command, 0); });
}
void test_kv_apply_rejects_before_state_mutation(void**) {
    TKv kv;
    auto entry = NewHoldedMessage<TKvLogEntry>();
    entry->KeySize = 1;
    entry->ValSize = UINT16_MAX;
    Reject([&] { kv.Write(entry, 1); });
    assert_int_equal(kv.LastAppliedIndex, 0);
    auto list = NewHoldedMessage<TReadKv>();
    assert_int_equal(kv.Read(list, 0)->Len, sizeof(TCommandResponse));
}
void test_valid_kv_payload_roundtrip(void**) {
    TKv kv;
    auto command = NewHoldedMessage<TWriteKv>(sizeof(TWriteKv) + 4);
    command->KeySize = 1;
    command->ValSize = 3;
    memcpy(command->TKvEntry::Data, "kabc", 4);
    kv.Write(kv.Prepare(command), 1);
    auto read = NewHoldedMessage<TReadKv>(sizeof(TReadKv) + 1);
    read->KeySize = 1;
    read->Data[0] = 'k';
    auto reply = kv.Read(read, 1).Cast<TCommandResponse>();
    assert_int_equal(reply->Len, sizeof(TCommandResponse) + 3);
    assert_memory_equal(reply->Data, "abc", 3);
    command->ValSize = 0;
    kv.Write(kv.Prepare(command), 2);
    assert_int_equal(kv.Read(read, 2)->Len, sizeof(TCommandResponse));
}
void test_dummy_read_rejects_missing_index(void**) {
    TDummyRsm rsm;
    auto command = NewHoldedMessage<TCommandRequest>();
    Reject([&] { rsm.Read(command, 0); });
}
void test_dummy_read_copies_only_log_data(void**) {
    TDummyRsm rsm;
    auto entry = NewHoldedMessage<TLogEntry>(sizeof(TLogEntry) + 3);
    memcpy(entry->Data, "abc", 3);
    rsm.Write(entry, 1);
    auto request = NewHoldedMessage<TCommandRequest>(sizeof(TCommandRequest) + sizeof(int64_t));
    int64_t index = 1;
    memcpy(request->Data, &index, sizeof(index));
    auto reply = rsm.Read(request, 1).Cast<TCommandResponse>();
    assert_int_equal(reply->Len, sizeof(TCommandResponse) + 3);
    assert_memory_equal(reply->Data, "abc", 3);
}
struct TEndpoint : INode {
    void Send(TMessageHolder<TMessage>) override {}
    void Drain() override {}
};
void test_invalid_request_is_rejected_before_queue_or_forward(void**) {
    for (bool write : {false, true}) {
        auto state = std::make_shared<TState>();
        auto raft = std::make_shared<TRaft>(state, 1, TNodeDict{});
        if (!write) raft->Become(EState::LEADER);
        auto kv = std::make_shared<TKv>();
        TRequestProcessor processor(raft, kv, {});
        auto endpoint = std::make_shared<TEndpoint>();
        std::weak_ptr<INode> weak = endpoint;
        auto command = NewHoldedMessage<TWriteKv>();
        command->Flags = write ? TCommandRequest::EWrite : TCommandRequest::EConsistent;
        command->KeySize = UINT16_MAX;
        Reject([&] { processor.OnCommandRequest(command, endpoint); });
        endpoint.reset();
        assert_true(weak.expired());
        assert_int_equal(state->LastLogIndex, 0);
    }
}
void test_oversized_factory_argument_is_not_truncated(void**) {
    if (sizeof(size_t) <= sizeof(uint32_t)) return;
    bool rejected = false;
    try { NewHoldedMessage<TCommandResponse>(size_t(UINT32_MAX) + 1); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert_true(rejected);
}
void test_read_application_error_does_not_stop_completion(void**) {
    struct TFailRead : TKv {
        TMessageHolder<TMessage> Read(TMessageHolder<TCommandRequest>, uint64_t) override {
            throw TMessageError("read output too large");
        }
    };
    struct TSink : INode {
        std::vector<uint32_t> Cookies;
        void Send(TMessageHolder<TMessage> message) override {
            auto reply = message.Cast<TCommandResponse>();
            assert_int_equal(reply->ErrorCode, 2);
            Cookies.push_back(reply->Cookie);
        }
        void Drain() override {}
    };
    auto state = std::make_shared<TState>();
    auto raft = std::make_shared<TRaft>(state, 1, TNodeDict{});
    raft->ProcessTimeout(ITimeSource::Time{});
    TRequestProcessor processor(raft, std::make_shared<TFailRead>(), {});
    auto endpoint = std::make_shared<TSink>();
    for (uint32_t flags : {0u, uint32_t(TCommandRequest::EConsistent)}) {
        auto command = NewHoldedMessage<TReadKv>();
        command->Flags = flags;
        command->Cookie = flags + 7;
        processor.OnCommandRequest(command, endpoint);
        processor.ProcessCommitted();
        processor.ProcessWaiting();
    }
    assert_int_equal(endpoint->Cookies.size(), 2);
    assert_int_equal(endpoint->Cookies[0], 7);
    assert_int_equal(endpoint->Cookies[1], 11);
    assert_int_equal(raft->CurrentStateName(), EState::LEADER);
}
#ifndef _WIN32
void test_invalid_committed_application_is_fatal(void**) {
    const auto child = fork();
    assert_true(child >= 0);
    if (child == 0) {
        std::set_terminate([] { _exit(73); });
        auto state = std::make_shared<TState>();
        auto entry = NewHoldedMessage<TKvLogEntry>();
        entry->KeySize = UINT16_MAX;
        state->Append(entry);
        auto raft = std::make_shared<TRaft>(state, 1, TNodeDict{});
        const_cast<TVolatileState*>(raft->GetVolatileState())->CommitIndex = 1;
        TRequestProcessor processor(raft, std::make_shared<TKv>(), {});
        try { processor.ProcessCommitted(); } catch (...) { _exit(74); }
        _exit(75);
    }
    int status = 0;
    assert_int_equal(waitpid(child, &status, 0), child);
    assert_true(WIFEXITED(status));
    assert_int_equal(WEXITSTATUS(status), 73);
}
#endif
}
int main() {
    const CMUnitTest tests[] = {
        cmocka_unit_test(test_kv_prepare_rejects_inconsistent_lengths),
        cmocka_unit_test(test_kv_read_rejects_short_key),
        cmocka_unit_test(test_kv_apply_rejects_before_state_mutation),
        cmocka_unit_test(test_valid_kv_payload_roundtrip),
        cmocka_unit_test(test_dummy_read_rejects_missing_index),
        cmocka_unit_test(test_dummy_read_copies_only_log_data),
        cmocka_unit_test(test_invalid_request_is_rejected_before_queue_or_forward),
        cmocka_unit_test(test_oversized_factory_argument_is_not_truncated),
        cmocka_unit_test(test_read_application_error_does_not_stop_completion),
#ifndef _WIN32
        cmocka_unit_test(test_invalid_committed_application_is_fatal),
#endif
    };
    return cmocka_run_group_tests(tests, nullptr, nullptr);
}
