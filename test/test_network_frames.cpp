#include <miniraft/net/server.h>
#include <algorithm>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
extern "C" {
#include <cmocka.h>
}

namespace {
// Immediate deterministic I/O: no sockets, sleeps, pollers or wall clock.
struct TStream {
    std::vector<char> Bytes;
    size_t Position = 0;
    size_t Chunk = SIZE_MAX;
    size_t MaxRequested = 0;
    unsigned Reads = 0;
    std::vector<char> Output;
    NNet::TValueTask<ssize_t> ReadSome(void* out, size_t length) {
        ++Reads;
        MaxRequested = std::max(MaxRequested, length);
        const auto count = std::min({length, Chunk, Bytes.size() - Position});
        if (count) memcpy(out, Bytes.data() + Position, count);
        Position += count;
        co_return count;
    }
    NNet::TValueTask<ssize_t> WriteSome(const void* data, size_t length) {
        const auto count = std::min(length, Chunk);
        auto bytes = static_cast<const char*>(data);
        Output.insert(Output.end(), bytes, bytes + count);
        co_return count;
    }
    void Add(uint32_t type, uint32_t length) {
        const char* a = reinterpret_cast<const char*>(&type);
        const char* b = reinterpret_cast<const char*>(&length);
        Bytes.insert(Bytes.end(), a, a + sizeof(type));
        Bytes.insert(Bytes.end(), b, b + sizeof(length));
    }
    template<class T> void Add(TMessageHolder<T> message) {
        auto data = reinterpret_cast<const char*>(message.Mes);
        Bytes.insert(Bytes.end(), data, data + message->Len);
    }
};
TMessageHolder<TMessage> Read(TStream& stream) {
    auto task = TMessageReader(stream).Read();
    assert_true(task.done());
    return task.await_resume();
}
bool Rejected(TStream& stream) {
    try { Read(stream); } catch (const std::runtime_error&) { return true; }
    return false;
}

void test_fragmented_header_and_body_are_read_exactly(void**) {
    TStream stream;
    stream.Chunk = 1;
    auto original = NewHoldedMessage(TRequestVoteRequest{.LastLogIndex = 7, .LastLogTerm = 2, .CandidateId = 1});
    stream.Add(original);
    bool complete = false;
    try {
        auto received = Read(stream).Cast<TRequestVoteRequest>();
        assert_int_equal(received->LastLogIndex, 7);
        assert_int_equal(received->LastLogTerm, 2);
        assert_int_equal(stream.Position, original->Len);
        complete = true;
    } catch (const std::runtime_error&) {}
    assert_true(complete);
}
void test_short_fixed_frame_is_rejected(void**) {
    TStream stream;
    stream.Add(uint32_t(EMessageType::REQUEST_VOTE_REQUEST), sizeof(TMessage));
    assert_true(Rejected(stream));
}
void test_unknown_and_unimplemented_types_are_rejected(void**) {
    for (auto type : {0u, 6u, 7u, 10u, UINT32_MAX}) {
        TStream stream;
        stream.Add(type, sizeof(TMessage));
        assert_true(Rejected(stream));
    }
}
void test_append_entries_rejects_non_log_child(void**) {
    TStream stream;
    stream.Add(NewHoldedMessage(TAppendEntriesRequest{.Nentries = 1}));
    stream.Add(NewHoldedMessage<TCommandRequest>());
    assert_true(Rejected(stream));
}
void test_append_entries_rejects_nested_append(void**) {
    TStream stream;
    stream.Add(NewHoldedMessage(TAppendEntriesRequest{.Nentries = 1}));
    stream.Add(NewHoldedMessage<TAppendEntriesRequest>());
    assert_true(Rejected(stream));
}
void test_oversized_frame_is_rejected_before_body_read(void**) {
    TStream stream;
    stream.Add(uint32_t(EMessageType::COMMAND_REQUEST), 16 * 1024 * 1024 + 1);
    assert_true(Rejected(stream));
    assert_true(stream.MaxRequested <= sizeof(TMessage));
}
void test_all_truncated_headers_are_rejected(void**) {
    for (size_t size = 0; size < sizeof(TMessage); ++size) {
        TStream stream;
        stream.Add(uint32_t(EMessageType::COMMAND_REQUEST), sizeof(TCommandRequest));
        stream.Bytes.resize(size);
        stream.Chunk = 1;
        assert_true(Rejected(stream));
    }
}
void test_invalid_lengths_are_rejected_before_body_read(void**) {
    for (auto length : {0u, 1u, 7u, 8u, 15u, UINT32_MAX}) {
        TStream stream;
        stream.Add(uint32_t(EMessageType::COMMAND_REQUEST), length);
        assert_true(Rejected(stream));
        assert_int_equal(stream.Reads, 1);
    }
    TStream stream;
    stream.Add(uint32_t(EMessageType::REQUEST_VOTE_REQUEST), sizeof(TRequestVoteRequest) + 1);
    assert_true(Rejected(stream));
    assert_int_equal(stream.Reads, 1);
}
void test_all_truncated_bodies_are_rejected(void**) {
    auto message = NewHoldedMessage<TRequestVoteRequest>();
    for (size_t size = sizeof(TMessage); size < message->Len; ++size) {
        TStream stream;
        stream.Add(message);
        stream.Bytes.resize(size);
        stream.Chunk = 3;
        assert_true(Rejected(stream));
    }
}
void test_count_and_minimum_batch_budget_are_checked_before_children(void**) {
    for (auto count : {4097u, UINT32_MAX}) {
        TStream stream;
        stream.Add(NewHoldedMessage(TAppendEntriesRequest{.Nentries = count}));
        assert_true(Rejected(stream));
        assert_int_equal(stream.Reads, 2);
    }
    TStream stream;
    stream.Add(NewHoldedMessage(TAppendEntriesRequest{.Nentries = 1}));
    TMessageLimits limits;
    limits.MaxBatchBytes = sizeof(TAppendEntriesRequest) + sizeof(TLogEntry) - 1;
    auto task = TMessageReader(stream, limits).Read();
    bool rejected = false;
    try { task.await_resume(); } catch (const TMessageError&) { rejected = true; }
    assert_true(rejected);
    assert_int_equal(stream.Reads, 2);
}
void test_aggregate_budget_is_shared_between_entries(void**) {
    for (bool enough : {false, true}) {
        TStream stream;
        stream.Add(NewHoldedMessage(TAppendEntriesRequest{.Nentries = 2}));
        stream.Add(NewHoldedMessage<TLogEntry>(sizeof(TLogEntry) + 1));
        stream.Add(NewHoldedMessage<TLogEntry>(sizeof(TLogEntry) + 1));
        TMessageLimits limits;
        limits.MaxBatchBytes = stream.Bytes.size() - (enough ? 0 : 1);
        auto task = TMessageReader(stream, limits).Read();
        bool rejected = false;
        try {
            auto message = task.await_resume();
            assert_int_equal(message.PayloadSize, 2);
        } catch (const TMessageError&) { rejected = true; }
        assert_int_equal(rejected, !enough);
        if (!enough) assert_int_equal(stream.Position,
            sizeof(TAppendEntriesRequest) + sizeof(TLogEntry) + 1 + sizeof(TMessage));
    }
}
void test_failed_reader_cannot_resume_at_another_frame(void**) {
    TStream stream;
    stream.Add(10, sizeof(TMessage));
    stream.Add(NewHoldedMessage<TCommandRequest>());
    TMessageReader reader(stream);
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto task = reader.Read();
        bool rejected = false;
        try { task.await_resume(); } catch (const TMessageError&) { rejected = true; }
        assert_true(rejected);
        assert_int_equal(stream.Position, sizeof(TMessage));
    }
}
void test_writer_validates_entire_batch_before_first_byte(void**) {
    for (int invalid = 0; invalid < 7; ++invalid) {
        auto root = NewHoldedMessage(TAppendEntriesRequest{.Nentries = 1});
        root.InitPayload(1);
        root.Payload[0] = NewHoldedMessage<TLogEntry>();
        TMessageLimits limits;
        if (invalid == 0) root->Nentries = 2;
        if (invalid == 1) root.Payload[0] = NewHoldedMessage<TCommandRequest>();
        if (invalid == 2) root.Payload[0] = NewHoldedMessage<TAppendEntriesRequest>();
        if (invalid == 3) limits.MaxBatchBytes = sizeof(TAppendEntriesRequest) + sizeof(TLogEntry) - 1;
        if (invalid == 4) root.Payload.reset();
        if (invalid == 5) root.Payload[0] = {};
        if (invalid == 6) root.Payload[0].InitPayload(1);
        TStream stream;
        auto task = TMessageWriter(stream, limits).Write(root);
        bool rejected = false;
        try { task.await_resume(); } catch (const TMessageError&) { rejected = true; }
        assert_true(rejected);
        assert_true(stream.Output.empty());
    }
}
void test_fragmented_roundtrip_keeps_consecutive_frame_boundaries(void**) {
    auto root = NewHoldedMessage(TAppendEntriesRequest{.Nentries = 2});
    root.InitPayload(2);
    root.Payload[0] = NewHoldedMessage<TLogEntry>();
    root.Payload[1] = NewHoldedMessage<TLogEntry>(sizeof(TLogEntry) + 3);
    memcpy(root.Payload[1].Cast<TLogEntry>()->Data, "abc", 3);
    TStream stream;
    stream.Chunk = 1;
    auto batch = TMessageWriter(stream).Write(root);
    batch.await_resume();
    auto command = TMessageWriter(stream).Write(NewHoldedMessage<TCommandRequest>());
    command.await_resume();
    stream.Bytes = stream.Output;
    auto received = Read(stream);
    assert_int_equal(received.PayloadSize, 2);
    auto entry = received.Payload[1].Cast<TLogEntry>();
    assert_memory_equal(entry->Data, "abc", 3);
    auto next = Read(stream).Cast<TCommandRequest>();
    assert_int_equal(next->Len, sizeof(TCommandRequest));
    assert_int_equal(stream.Position, stream.Bytes.size());
}
}

int main() {
    const CMUnitTest tests[] = {
        cmocka_unit_test(test_fragmented_header_and_body_are_read_exactly),
        cmocka_unit_test(test_short_fixed_frame_is_rejected),
        cmocka_unit_test(test_unknown_and_unimplemented_types_are_rejected),
        cmocka_unit_test(test_append_entries_rejects_non_log_child),
        cmocka_unit_test(test_append_entries_rejects_nested_append),
        cmocka_unit_test(test_oversized_frame_is_rejected_before_body_read),
        cmocka_unit_test(test_all_truncated_headers_are_rejected),
        cmocka_unit_test(test_invalid_lengths_are_rejected_before_body_read),
        cmocka_unit_test(test_all_truncated_bodies_are_rejected),
        cmocka_unit_test(test_count_and_minimum_batch_budget_are_checked_before_children),
        cmocka_unit_test(test_aggregate_budget_is_shared_between_entries),
        cmocka_unit_test(test_failed_reader_cannot_resume_at_another_frame),
        cmocka_unit_test(test_writer_validates_entire_batch_before_first_byte),
        cmocka_unit_test(test_fragmented_roundtrip_keeps_consecutive_frame_boundaries),
    };
    return cmocka_run_group_tests(tests, nullptr, nullptr);
}
