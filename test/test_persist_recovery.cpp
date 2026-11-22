#include <miniraft/persist.h>
#include <filesystem>
#include <chrono>
#include <map>
#include <iterator>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
extern "C" {
#include <cmocka.h>
}

namespace {
namespace fs = std::filesystem;
struct TFiles {
    fs::path Directory;
    std::string Name;
    TFiles() {
        static unsigned counter = 0;
        Directory = fs::temp_directory_path() /
            ("miniraft-recovery-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(counter++));
        assert_true(fs::create_directory(Directory));
        Name = (Directory / "raft").string();
    }
    ~TFiles() { std::error_code ignored; fs::remove_all(Directory, ignored); }
    fs::path Path(const std::string& kind) const { return Name + "." + kind + ".1"; }
    void Seed() {
        TDiskState state(Name, 1);
        state.CurrentTerm = 7;
        state.VotedFor = 2;
        for (uint64_t term : {2, 5}) {
            auto entry = NewHoldedMessage<TLogEntry>();
            entry->Term = term;
            state.Append(entry);
        }
    }
    template<class T> void Overwrite(const std::string& kind, uint64_t offset, T value) {
        std::fstream file(Path(kind), std::ios::in | std::ios::out | std::ios::binary);
        file.seekp(offset);
        file.write(reinterpret_cast<const char*>(&value), sizeof(value));
        file.flush();
        assert_true(bool(file));
    }
    void RejectRecovery() {
        auto contents = [&] {
            std::map<std::string, std::string> result;
            for (auto& file : fs::directory_iterator(Directory)) {
                std::ifstream input(file.path(), std::ios::binary);
                result[file.path().filename().string()] =
                    std::string(std::istreambuf_iterator<char>(input), {});
            }
            return result;
        };
        const auto before = contents();
        bool rejected = false;
        try { TDiskState state(Name, 1); }
        catch (const std::runtime_error&) { rejected = true; }
        assert_true(rejected);
        assert_true(before == contents());
    }
};

void test_truncated_metadata_is_not_reset(void**) {
    for (uint64_t size : {0, 1, 8, 16, 19}) {
        TFiles files;
        files.Seed();
        fs::resize_file(files.Path("state"), size);
        files.RejectRecovery();
        assert_int_equal(fs::file_size(files.Path("state")), size);
    }
}
void test_oversized_metadata_is_rejected(void**) {
    TFiles files;
    files.Seed();
    fs::resize_file(files.Path("state"), 21);
    files.RejectRecovery();
    assert_int_equal(fs::file_size(files.Path("state")), 21);
}
void test_missing_storage_is_not_recreated(void**) {
    for (const std::string kind : {"state", "index", "entries"}) {
        TFiles files;
        files.Seed();
        fs::remove(files.Path(kind));
        files.RejectRecovery();
        assert_false(fs::exists(files.Path(kind)));
    }
}
void test_short_index_is_rejected(void**) {
    TFiles files;
    files.Seed();
    fs::resize_file(files.Path("index"), sizeof(uint64_t));
    files.RejectRecovery();
}
void test_short_entry_is_rejected(void**) {
    TFiles files;
    files.Seed();
    fs::resize_file(files.Path("entries"), sizeof(TLogEntry) + 4);
    files.RejectRecovery();
}
void test_invalid_entry_header_is_rejected(void**) {
    for (uint32_t len : {0u, 7u, 8u, uint32_t(sizeof(TLogEntry) - 1), UINT32_MAX}) {
        TFiles files;
        files.Seed();
        files.Overwrite("entries", sizeof(uint32_t), len);
        files.RejectRecovery();
    }
    TFiles files;
    files.Seed();
    files.Overwrite("entries", 0, uint32_t(EMessageType::COMMAND_REQUEST));
    files.RejectRecovery();
}
void test_invalid_offsets_are_rejected(void**) {
    for (uint64_t offset : {uint64_t(0), uint64_t(1), UINT64_MAX}) {
        TFiles files;
        files.Seed();
        files.Overwrite("index", sizeof(uint64_t), offset);
        files.RejectRecovery();
    }
}
void test_invalid_term_is_rejected(void**) {
    TFiles files;
    files.Seed();
    files.Overwrite("state", sizeof(uint64_t), uint64_t(0));
    files.RejectRecovery();
}
void test_invalid_log_count_is_rejected(void**) {
    TFiles files;
    files.Seed();
    files.Overwrite("state", 0, UINT64_MAX);
    files.RejectRecovery();
}
void test_truncate_updates_last_term_and_restarts(void**) {
    TFiles files;
    files.Seed();
    {
        TDiskState state(files.Name, 1);
        state.RemoveLast();
        assert_int_equal(state.LastLogIndex, 1);
        assert_int_equal(state.LastLogTerm, 2);
    }
    {
        TDiskState state(files.Name, 1);
        assert_int_equal(state.CurrentTerm, 7);
        assert_int_equal(state.VotedFor, 2);
        assert_int_equal(state.LastLogTerm, 2);
        state.RemoveLast();
        assert_int_equal(state.LastLogIndex, 0);
        assert_int_equal(state.LastLogTerm, 0);
    }
    {
        // Logical truncation leaves a suffix on disk. Reopen it as an empty
        // log, then ensure the replacement really starts at byte zero.
        TDiskState state(files.Name, 1);
        assert_int_equal(state.LastLogIndex, 0);
        assert_int_equal(state.LastLogTerm, 0);
        auto entry = NewHoldedMessage<TLogEntry>();
        entry->Term = 7;
        state.Append(entry);
        assert_int_equal(state.Get(1)->Term, 7);
    }
    TDiskState recovered(files.Name, 1);
    assert_int_equal(recovered.LastLogIndex, 1);
    assert_int_equal(recovered.LastLogTerm, 7);
    assert_int_equal(recovered.Get(1)->Term, 7);
    assert_int_equal(recovered.VotedFor, 2);
    // Also replace an empty log without reopening: the shared stream position
    // is still at the end of the removed entry, rather than at byte zero.
    recovered.RemoveLast();
    auto replacement = NewHoldedMessage<TLogEntry>();
    replacement->Term = 6;
    recovered.Append(replacement);
    assert_int_equal(recovered.Get(1)->Term, 6);
    assert_int_equal(recovered.LastLogTerm, 6);
}
void test_zero_log_entry_terms_are_rejected_without_mutation(void**) {
    for (uint64_t index : {0, 1}) {
        TFiles files;
        files.Seed();
        files.Overwrite("entries", index * sizeof(TLogEntry) + sizeof(TMessage), uint64_t(0));
        files.RejectRecovery();
    }
}
void test_log_terms_newer_than_current_term_are_rejected(void**) {
    for (uint64_t term : {uint64_t(8), UINT64_MAX}) {
        TFiles files;
        files.Seed();
        files.Overwrite("entries", sizeof(TLogEntry) + sizeof(TMessage), term);
        files.RejectRecovery();
    }
    TFiles files;
    files.Seed();
    files.Overwrite("state", sizeof(uint64_t), uint64_t(4));
    files.RejectRecovery();
}
void test_decreasing_log_terms_are_rejected(void**) {
    for (uint64_t index : {0, 1}) {
        TFiles files;
        files.Seed();
        files.Overwrite("entries", index * sizeof(TLogEntry) + sizeof(TMessage), uint64_t(index == 0 ? 6 : 1));
        files.RejectRecovery();
    }
}
void test_equal_and_skipped_terms_and_full_width_current_term_recover(void**) {
    TFiles files;
    {
        TDiskState state(files.Name, 1);
        state.CurrentTerm = UINT64_MAX;
        state.VotedFor = 2;
        for (uint64_t term : {uint64_t(1), uint64_t(1), uint64_t(5), UINT64_MAX, UINT64_MAX}) {
            auto entry = NewHoldedMessage<TLogEntry>();
            entry->Term = term;
            state.Append(entry);
        }
    }
    TDiskState recovered(files.Name, 1);
    assert_true(recovered.CurrentTerm == UINT64_MAX);
    assert_true(recovered.LastLogTerm == UINT64_MAX);
    assert_int_equal(recovered.LastLogIndex, 5);
    assert_int_equal(recovered.VotedFor, 2);
    assert_int_equal(recovered.Get(1)->Term, 1);
    assert_int_equal(recovered.Get(2)->Term, 1);
    assert_int_equal(recovered.Get(3)->Term, 5);
}
void test_invalid_terms_in_logically_removed_suffix_are_ignored(void**) {
    TFiles files;
    files.Seed();
    {
        TDiskState state(files.Name, 1);
        state.RemoveLast();
    }
    files.Overwrite("entries", sizeof(TLogEntry) + sizeof(TMessage), uint64_t(0));
    TDiskState recovered(files.Name, 1);
    assert_int_equal(recovered.LastLogIndex, 1);
    assert_int_equal(recovered.LastLogTerm, 2);
    auto entry = NewHoldedMessage<TLogEntry>();
    entry->Term = 7;
    recovered.Append(entry);
    assert_int_equal(recovered.LastLogIndex, 2);
    assert_int_equal(recovered.Get(2)->Term, 7);
}

void test_live_storage_failure_is_reported(void**) {
    TFiles files;
    files.Seed();
    TDiskState state(files.Name, 1);
    fs::resize_file(files.Path("entries"), 0);
    bool rejected = false;
    try { state.Get(1); } catch (const std::runtime_error&) { rejected = true; }
    assert_true(rejected);
}
void test_live_invalid_log_term_is_reported(void**) {
    for (uint64_t term : {uint64_t(0), uint64_t(8)}) {
        TFiles files;
        files.Seed();
        TDiskState state(files.Name, 1);
        files.Overwrite("entries", sizeof(TMessage), term);
        bool rejected = false;
        try { state.Get(1); } catch (const std::runtime_error&) { rejected = true; }
        assert_true(rejected);
    }
}
}

int main() {
    const CMUnitTest tests[] = {
        cmocka_unit_test(test_truncated_metadata_is_not_reset),
        cmocka_unit_test(test_oversized_metadata_is_rejected),
        cmocka_unit_test(test_missing_storage_is_not_recreated),
        cmocka_unit_test(test_short_index_is_rejected),
        cmocka_unit_test(test_short_entry_is_rejected),
        cmocka_unit_test(test_invalid_entry_header_is_rejected),
        cmocka_unit_test(test_invalid_offsets_are_rejected),
        cmocka_unit_test(test_invalid_term_is_rejected),
        cmocka_unit_test(test_invalid_log_count_is_rejected),
        cmocka_unit_test(test_truncate_updates_last_term_and_restarts),
        cmocka_unit_test(test_live_storage_failure_is_reported),
        cmocka_unit_test(test_zero_log_entry_terms_are_rejected_without_mutation),
        cmocka_unit_test(test_log_terms_newer_than_current_term_are_rejected),
        cmocka_unit_test(test_decreasing_log_terms_are_rejected),
        cmocka_unit_test(test_equal_and_skipped_terms_and_full_width_current_term_recover),
        cmocka_unit_test(test_invalid_terms_in_logically_removed_suffix_are_ignored),
        cmocka_unit_test(test_live_invalid_log_term_is_reported),
    };
    return cmocka_run_group_tests(tests, nullptr, nullptr);
}
