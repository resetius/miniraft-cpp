#include "../examples/kv.h"
#include "../examples/kv_messages.h"
#include <algorithm>
#include <string>
#include <vector>
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
TMessageHolder<TWriteKv> SetKv(const std::string& key, const std::string& value) {
    auto command = NewHoldedMessage<TWriteKv>(sizeof(TWriteKv) + key.size() + value.size());
    command->Flags = TCommandRequest::EWrite;
    command->KeySize = key.size();
    command->ValSize = value.size();
    memcpy(command->TKvEntry::Data, key.data(), key.size());
    memcpy(command->TKvEntry::Data + key.size(), value.data(), value.size());
    return command;
}
std::string ListKv(TKv& kv) {
    auto reply = kv.Read(NewHoldedMessage<TReadKv>(), 0).Cast<TCommandResponse>();
    std::string text(reply->Data, reply->Len - sizeof(TCommandResponse));
    std::vector<std::string> lines;
    for (size_t start = 0, end; start < text.size(); start = end + 1) {
        end = text.find('\n', start);
        lines.push_back(text.substr(start, end - start));
    }
    std::sort(lines.begin(), lines.end());
    std::string sorted;
    for (const auto& line : lines) sorted += line + "\n";
    return sorted;
}
void test_kv_snapshot_roundtrip(void**) {
    TKv kv;
    uint64_t index = 0;
    for (auto [key, value] : std::initializer_list<std::pair<std::string, std::string>>{
             {"alpha", "1"}, {"long-key", std::string(1000, 'v')}, {"gone", "x"}, {"alpha", "2"}, {"gone", ""}}) {
        kv.Write(kv.Prepare(SetKv(key, value)), ++index);
    }
    auto snapshot = kv.Snapshot();
    assert_int_equal(snapshot->LastIncludedIndex, index);
    assert_int_equal(snapshot.PayloadSize, 2);
    TKv restored;
    restored.Write(kv.Prepare(SetKv("stale", "y")), 1);
    restored.Apply(snapshot);
    assert_string_equal(ListKv(restored).c_str(), ListKv(kv).c_str());
    assert_string_equal(ListKv(restored).c_str(), ("alpha = 2\nlong-key = " + std::string(1000, 'v') + "\n").c_str());
}
void test_kv_invalid_snapshot_leaves_state_unchanged(void**) {
    TKv kv;
    kv.Write(kv.Prepare(SetKv("keep", "me")), 1);
    const auto before = ListKv(kv);
    for (uint16_t keySize : {uint16_t(3), uint16_t(UINT16_MAX)}) {
        auto snapshot = kv.Snapshot();
        auto bad = NewHoldedMessage<TLogEntry>(sizeof(TLogEntry) + sizeof(TKvEntry) + 2);
        const TKvEntry header{.KeySize = keySize, .ValSize = 0};
        memcpy(bad->Data, &header, sizeof(header));
        snapshot.InitPayload(2);
        snapshot.Payload[0] = kv.Snapshot().Payload[0];
        snapshot.Payload[1] = bad;
        Reject([&] { kv.Apply(snapshot); });
        assert_string_equal(ListKv(kv).c_str(), before.c_str());
    }
    auto snapshot = kv.Snapshot();
    snapshot.Payload[0] = NewHoldedMessage<TLogEntry>();
    Reject([&] { kv.Apply(snapshot); });
    assert_string_equal(ListKv(kv).c_str(), before.c_str());
    // A key without a value: Write would delete it, so the image cannot hold it.
    auto empty = NewHoldedMessage<TLogEntry>(sizeof(TLogEntry) + sizeof(TKvEntry) + 3);
    const TKvEntry header{.KeySize = 3, .ValSize = 0};
    memcpy(empty->Data, &header, sizeof(header));
    memcpy(empty->Data + sizeof(header), "key", 3);
    snapshot.Payload[0] = empty;
    Reject([&] { kv.Apply(snapshot); });
    assert_string_equal(ListKv(kv).c_str(), before.c_str());
}
void test_kv_restores_from_a_stored_snapshot(void**) {
    TKv source;
    source.Write(source.Prepare(SetKv("key", "value")), 3);
    auto image = source.Snapshot();
    image->LastIncludedTerm = 1;
    auto state = std::make_shared<TState>();
    state->InstallSnapshot(image);
    auto raft = std::make_shared<TRaft>(state, 1, TNodeDict{});
    auto kv = std::make_shared<TKv>();
    TRequestProcessor processor(raft, kv, {});
    processor.ProcessCommitted();
    assert_int_equal(kv->LastAppliedIndex, 3);
    assert_string_equal(ListKv(*kv).c_str(), "key = value\n");
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
void test_invalid_committed_application_is_reported(void**) {
    auto state = std::make_shared<TState>();
    auto entry = NewHoldedMessage<TKvLogEntry>();
    entry->KeySize = UINT16_MAX;
    state->Append(entry);
    auto raft = std::make_shared<TRaft>(state, 1, TNodeDict{});
    const_cast<TVolatileState*>(raft->GetVolatileState())->CommitIndex = 1;
    auto kv = std::make_shared<TKv>();
    TRequestProcessor processor(raft, kv, {});
    Reject([&] { processor.ProcessCommitted(); });
    assert_int_equal(kv->LastAppliedIndex, 0);
}
}
int main() {
    const CMUnitTest tests[] = {
        cmocka_unit_test(test_kv_prepare_rejects_inconsistent_lengths),
        cmocka_unit_test(test_kv_read_rejects_short_key),
        cmocka_unit_test(test_kv_apply_rejects_before_state_mutation),
        cmocka_unit_test(test_valid_kv_payload_roundtrip),
        cmocka_unit_test(test_kv_snapshot_roundtrip),
        cmocka_unit_test(test_kv_invalid_snapshot_leaves_state_unchanged),
        cmocka_unit_test(test_kv_restores_from_a_stored_snapshot),
        cmocka_unit_test(test_dummy_read_rejects_missing_index),
        cmocka_unit_test(test_dummy_read_copies_only_log_data),
        cmocka_unit_test(test_invalid_request_is_rejected_before_queue_or_forward),
        cmocka_unit_test(test_oversized_factory_argument_is_not_truncated),
        cmocka_unit_test(test_read_application_error_does_not_stop_completion),
        cmocka_unit_test(test_invalid_committed_application_is_reported),
    };
    return cmocka_run_group_tests(tests, nullptr, nullptr);
}
