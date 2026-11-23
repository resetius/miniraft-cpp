#include <miniraft/persist.h>
#include <filesystem>
#include <chrono>
#include <functional>
#include <map>
#include <iterator>
#include <gtest/gtest.h>

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
        EXPECT_TRUE(fs::create_directory(Directory));
        if (!fs::is_directory(Directory)) {
            throw std::runtime_error("cannot create " + Directory.string());
        }
        Name = (Directory / "raft").string();
    }
    ~TFiles() { std::error_code ignored; fs::remove_all(Directory, ignored); }
    fs::path Path(const std::string& kind) const { return Name + "." + kind + ".1"; }
    fs::path Path(const std::string& kind, uint64_t snapshot) const {
        return Name + "." + kind + ".1." + std::to_string(snapshot);
    }
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
        OverwriteAt(Path(kind), offset, value);
    }
    template<class T> void OverwriteAt(const fs::path& path, uint64_t offset, T value) {
        std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
        file.seekp(offset);
        file.write(reinterpret_cast<const char*>(&value), sizeof(value));
        file.flush();
        ASSERT_TRUE(bool(file));
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
        ASSERT_TRUE(rejected);
        ASSERT_TRUE(before == contents());
    }
};

TEST(PersistRecovery, truncated_metadata_is_not_reset) {
    for (uint64_t size : {0, 1, 8, 16, 19}) {
        TFiles files;
        files.Seed();
        fs::resize_file(files.Path("state"), size);
        ASSERT_NO_FATAL_FAILURE(files.RejectRecovery());
        ASSERT_EQ(fs::file_size(files.Path("state")), size);
    }
}
TEST(PersistRecovery, oversized_metadata_is_rejected) {
    TFiles files;
    files.Seed();
    fs::resize_file(files.Path("state"), 21);
    ASSERT_NO_FATAL_FAILURE(files.RejectRecovery());
    ASSERT_EQ(fs::file_size(files.Path("state")), 21);
}
TEST(PersistRecovery, missing_storage_is_not_recreated) {
    for (const std::string kind : {"state", "index", "entries"}) {
        TFiles files;
        files.Seed();
        fs::remove(files.Path(kind));
        ASSERT_NO_FATAL_FAILURE(files.RejectRecovery());
        ASSERT_FALSE(fs::exists(files.Path(kind)));
    }
}
TEST(PersistRecovery, short_index_is_rejected) {
    TFiles files;
    files.Seed();
    fs::resize_file(files.Path("index"), sizeof(uint64_t));
    ASSERT_NO_FATAL_FAILURE(files.RejectRecovery());
}
TEST(PersistRecovery, short_entry_is_rejected) {
    TFiles files;
    files.Seed();
    fs::resize_file(files.Path("entries"), sizeof(TLogEntry) + 4);
    ASSERT_NO_FATAL_FAILURE(files.RejectRecovery());
}
TEST(PersistRecovery, invalid_entry_header_is_rejected) {
    for (uint32_t len : {0u, 7u, 8u, uint32_t(sizeof(TLogEntry) - 1), UINT32_MAX}) {
        TFiles files;
        files.Seed();
        files.Overwrite("entries", sizeof(uint32_t), len);
        ASSERT_NO_FATAL_FAILURE(files.RejectRecovery());
    }
    TFiles files;
    files.Seed();
    files.Overwrite("entries", 0, uint32_t(EMessageType::COMMAND_REQUEST));
    ASSERT_NO_FATAL_FAILURE(files.RejectRecovery());
}
TEST(PersistRecovery, invalid_offsets_are_rejected) {
    for (uint64_t offset : {uint64_t(0), uint64_t(1), UINT64_MAX}) {
        TFiles files;
        files.Seed();
        files.Overwrite("index", sizeof(uint64_t), offset);
        ASSERT_NO_FATAL_FAILURE(files.RejectRecovery());
    }
}
TEST(PersistRecovery, invalid_term_is_rejected) {
    TFiles files;
    files.Seed();
    files.Overwrite("state", sizeof(uint64_t), uint64_t(0));
    ASSERT_NO_FATAL_FAILURE(files.RejectRecovery());
}
TEST(PersistRecovery, invalid_log_count_is_rejected) {
    TFiles files;
    files.Seed();
    files.Overwrite("state", 0, UINT64_MAX);
    ASSERT_NO_FATAL_FAILURE(files.RejectRecovery());
}
TEST(PersistRecovery, truncate_updates_last_term_and_restarts) {
    TFiles files;
    files.Seed();
    {
        TDiskState state(files.Name, 1);
        state.RemoveLast();
        ASSERT_EQ(state.LastLogIndex, 1);
        ASSERT_EQ(state.LastLogTerm, 2);
    }
    {
        TDiskState state(files.Name, 1);
        ASSERT_EQ(state.CurrentTerm, 7);
        ASSERT_EQ(state.VotedFor, 2);
        ASSERT_EQ(state.LastLogTerm, 2);
        state.RemoveLast();
        ASSERT_EQ(state.LastLogIndex, 0);
        ASSERT_EQ(state.LastLogTerm, 0);
    }
    {
        // Logical truncation leaves a suffix on disk. Reopen it as an empty
        // log, then ensure the replacement really starts at byte zero.
        TDiskState state(files.Name, 1);
        ASSERT_EQ(state.LastLogIndex, 0);
        ASSERT_EQ(state.LastLogTerm, 0);
        auto entry = NewHoldedMessage<TLogEntry>();
        entry->Term = 7;
        state.Append(entry);
        ASSERT_EQ(state.Get(1)->Term, 7);
    }
    TDiskState recovered(files.Name, 1);
    ASSERT_EQ(recovered.LastLogIndex, 1);
    ASSERT_EQ(recovered.LastLogTerm, 7);
    ASSERT_EQ(recovered.Get(1)->Term, 7);
    ASSERT_EQ(recovered.VotedFor, 2);
    // Also replace an empty log without reopening: the shared stream position
    // is still at the end of the removed entry, rather than at byte zero.
    recovered.RemoveLast();
    auto replacement = NewHoldedMessage<TLogEntry>();
    replacement->Term = 6;
    recovered.Append(replacement);
    ASSERT_EQ(recovered.Get(1)->Term, 6);
    ASSERT_EQ(recovered.LastLogTerm, 6);
}
TEST(PersistRecovery, zero_log_entry_terms_are_rejected_without_mutation) {
    for (uint64_t index : {0, 1}) {
        TFiles files;
        files.Seed();
        files.Overwrite("entries", index * sizeof(TLogEntry) + sizeof(TMessage), uint64_t(0));
        ASSERT_NO_FATAL_FAILURE(files.RejectRecovery());
    }
}
TEST(PersistRecovery, log_terms_newer_than_current_term_are_rejected) {
    for (uint64_t term : {uint64_t(8), UINT64_MAX}) {
        TFiles files;
        files.Seed();
        files.Overwrite("entries", sizeof(TLogEntry) + sizeof(TMessage), term);
        ASSERT_NO_FATAL_FAILURE(files.RejectRecovery());
    }
    TFiles files;
    files.Seed();
    files.Overwrite("state", sizeof(uint64_t), uint64_t(4));
    ASSERT_NO_FATAL_FAILURE(files.RejectRecovery());
}
TEST(PersistRecovery, decreasing_log_terms_are_rejected) {
    for (uint64_t index : {0, 1}) {
        TFiles files;
        files.Seed();
        files.Overwrite("entries", index * sizeof(TLogEntry) + sizeof(TMessage), uint64_t(index == 0 ? 6 : 1));
        ASSERT_NO_FATAL_FAILURE(files.RejectRecovery());
    }
}
TEST(PersistRecovery, equal_and_skipped_terms_and_full_width_current_term_recover) {
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
    ASSERT_TRUE(recovered.CurrentTerm == UINT64_MAX);
    ASSERT_TRUE(recovered.LastLogTerm == UINT64_MAX);
    ASSERT_EQ(recovered.LastLogIndex, 5);
    ASSERT_EQ(recovered.VotedFor, 2);
    ASSERT_EQ(recovered.Get(1)->Term, 1);
    ASSERT_EQ(recovered.Get(2)->Term, 1);
    ASSERT_EQ(recovered.Get(3)->Term, 5);
}
TEST(PersistRecovery, invalid_terms_in_logically_removed_suffix_are_ignored) {
    TFiles files;
    files.Seed();
    {
        TDiskState state(files.Name, 1);
        state.RemoveLast();
    }
    files.Overwrite("entries", sizeof(TLogEntry) + sizeof(TMessage), uint64_t(0));
    TDiskState recovered(files.Name, 1);
    ASSERT_EQ(recovered.LastLogIndex, 1);
    ASSERT_EQ(recovered.LastLogTerm, 2);
    auto entry = NewHoldedMessage<TLogEntry>();
    entry->Term = 7;
    recovered.Append(entry);
    ASSERT_EQ(recovered.LastLogIndex, 2);
    ASSERT_EQ(recovered.Get(2)->Term, 7);
}

TMessageHolder<TInstallSnapshotRequest> Image(uint64_t index, uint64_t term, std::initializer_list<std::string> values) {
    auto image = NewHoldedMessage(TMessageEx{}, TInstallSnapshotRequest{
        .LastIncludedIndex = index, .LastIncludedTerm = term, .Nentries = uint32_t(values.size()), .Done = 1});
    image.InitPayload(values.size());
    uint32_t i = 0;
    for (const auto& value : values) {
        auto record = NewHoldedMessage<TLogEntry>(sizeof(TLogEntry) + value.size());
        memcpy(record->Data, value.data(), value.size());
        image.Payload[i++] = record;
    }
    return image;
}
std::vector<std::string> Records(const TMessageHolder<TInstallSnapshotRequest>& image) {
    std::vector<std::string> values;
    for (uint32_t i = 0; i < image.PayloadSize; ++i) {
        auto record = image.Payload[i].Mes;
        values.emplace_back(reinterpret_cast<const TLogEntry*>(record)->Data, record->Len - sizeof(TLogEntry));
    }
    return values;
}
void AppendTerm(TDiskState& state, uint64_t term) {
    auto entry = NewHoldedMessage<TLogEntry>();
    entry->Term = term;
    state.Append(entry);
}
TEST(PersistRecovery, compacted_log_and_image_survive_restart) {
    TFiles files;
    files.Seed();
    ASSERT_EQ(fs::file_size(files.Path("state")), 20);
    {
        TDiskState state(files.Name, 1);
        AppendTerm(state, 7);
        state.InstallSnapshot(Image(2, 5, {"a", "b"}));
        ASSERT_EQ(state.SnapshotIndex, 2);
        ASSERT_EQ(state.LastLogIndex, 3);
        ASSERT_EQ(state.LastLogTerm, 7);
        ASSERT_FALSE(state.Get(2));
        ASSERT_EQ(state.Get(3)->Term, 7);
    }
    ASSERT_EQ(fs::file_size(files.Path("state")), 36);
    ASSERT_FALSE(fs::exists(files.Path("entries")));
    ASSERT_FALSE(fs::exists(files.Path("index")));
    {
        TDiskState state(files.Name, 1);
        ASSERT_EQ(state.SnapshotIndex, 2);
        ASSERT_EQ(state.SnapshotTerm, 5);
        ASSERT_EQ(state.LastLogIndex, 3);
        ASSERT_EQ(state.LogTerm(2), 5);
        ASSERT_EQ(state.Get(3)->Term, 7);
        ASSERT_TRUE(Records(state.GetSnapshot()) == (std::vector<std::string>{"a", "b"}));
        AppendTerm(state, 7);
        state.RemoveLast();
        state.RemoveLast();
        ASSERT_EQ(state.LastLogIndex, 2);
        ASSERT_EQ(state.LastLogTerm, 5);
        // Removal never reaches into the snapshot.
        state.RemoveLast();
        ASSERT_EQ(state.LastLogIndex, 2);
        AppendTerm(state, 7);
        // An image from a leader that the local log does not match replaces the whole log.
        state.InstallSnapshot(Image(5, 7, {"c"}));
        ASSERT_EQ(state.LastLogIndex, 5);
        ASSERT_EQ(state.LastLogTerm, 7);
    }
    ASSERT_FALSE(fs::exists(files.Path("snapshot", 2)));
    TDiskState state(files.Name, 1);
    ASSERT_EQ(state.SnapshotIndex, 5);
    ASSERT_EQ(state.LastLogIndex, 5);
    ASSERT_TRUE(Records(state.GetSnapshot()) == std::vector<std::string>{"c"});
    AppendTerm(state, 7);
    ASSERT_EQ(state.Get(6)->Term, 7);
    bool rejected = false;
    try { state.InstallSnapshot(Image(5, 7, {})); } catch (const std::invalid_argument&) { rejected = true; }
    ASSERT_TRUE(rejected);
}
TEST(PersistRecovery, interrupted_snapshot_publication_recovers_one_side) {
    // Before the metadata commit: the files of the new index are left over.
    {
        TFiles files;
        files.Seed();
        { TDiskState state(files.Name, 1); state.InstallSnapshot(Image(1, 2, {"a"})); }
        for (const std::string kind : {"entries", "index", "snapshot"}) {
            fs::copy_file(files.Path(kind, 1), files.Path(kind, 2));
        }
        fs::resize_file(files.Path("snapshot", 2), 3);
        TDiskState state(files.Name, 1);
        ASSERT_EQ(state.SnapshotIndex, 1);
        ASSERT_EQ(state.LastLogIndex, 2);
        ASSERT_EQ(state.Get(2)->Term, 5);
        for (const std::string kind : {"entries", "index", "snapshot"}) {
            ASSERT_FALSE(fs::exists(files.Path(kind, 2)));
        }
    }
    // After it: the files of the previous index are left over.
    TFiles files;
    files.Seed();
    fs::copy_file(files.Path("entries"), files.Path("entries") += ".old");
    fs::copy_file(files.Path("index"), files.Path("index") += ".old");
    { TDiskState state(files.Name, 1); state.InstallSnapshot(Image(1, 2, {"a"})); }
    fs::rename(files.Path("entries") += ".old", files.Path("entries"));
    fs::rename(files.Path("index") += ".old", files.Path("index"));
    // Names the store never writes are not its leftovers.
    for (const std::string name : {".entries.1.07", ".index.1.0", ".snapshot.1.2x", ".entries.10"}) {
        std::ofstream(files.Name + name) << "foreign";
    }
    TDiskState state(files.Name, 1);
    ASSERT_EQ(state.SnapshotIndex, 1);
    ASSERT_EQ(state.Get(2)->Term, 5);
    ASSERT_FALSE(fs::exists(files.Path("entries")));
    ASSERT_FALSE(fs::exists(files.Path("index")));
    for (const std::string name : {".entries.1.07", ".index.1.0", ".snapshot.1.2x", ".entries.10"}) {
        ASSERT_TRUE(fs::exists(files.Name + name));
    }
}
TEST(PersistRecovery, incomplete_or_corrupt_compacted_store_is_rejected) {
    const std::vector<std::function<void(TFiles&)>> damages = {
        [](TFiles& f) { fs::remove(f.Path("snapshot", 1)); },
        [](TFiles& f) { fs::remove(f.Path("entries", 1)); },
        [](TFiles& f) { fs::remove(f.Path("index", 1)); },
        [](TFiles& f) { fs::remove(f.Path("state")); },
        [](TFiles& f) { fs::resize_file(f.Path("snapshot", 1), fs::file_size(f.Path("snapshot", 1)) - 1); },
        [](TFiles& f) { fs::resize_file(f.Path("snapshot", 1), fs::file_size(f.Path("snapshot", 1)) + 1); },
        [](TFiles& f) { f.OverwriteAt(f.Path("snapshot", 1), sizeof(TMessageEx), uint64_t(2)); },
        [](TFiles& f) { f.Overwrite("state", 20, uint64_t(3)); },
        [](TFiles& f) { f.Overwrite("state", 28, uint64_t(8)); },
        [](TFiles& f) { f.Overwrite("state", 28, uint64_t(0)); },
    };
    for (const auto& damage : damages) {
        TFiles files;
        files.Seed();
        { TDiskState state(files.Name, 1); state.InstallSnapshot(Image(1, 2, {"a"})); }
        damage(files);
        ASSERT_NO_FATAL_FAILURE(files.RejectRecovery());
    }
}

TEST(PersistRecovery, live_storage_failure_is_reported) {
    TFiles files;
    files.Seed();
    TDiskState state(files.Name, 1);
    fs::resize_file(files.Path("entries"), 0);
    bool rejected = false;
    try { state.Get(1); } catch (const std::runtime_error&) { rejected = true; }
    ASSERT_TRUE(rejected);
}
TEST(PersistRecovery, live_invalid_log_term_is_reported) {
    for (uint64_t term : {uint64_t(0), uint64_t(8)}) {
        TFiles files;
        files.Seed();
        TDiskState state(files.Name, 1);
        files.Overwrite("entries", sizeof(TMessage), term);
        bool rejected = false;
        try { state.Get(1); } catch (const std::runtime_error&) { rejected = true; }
        ASSERT_TRUE(rejected);
    }
}
}

