#include <miniraft/durable.h>
#include <miniraft/persist.h>
#include <miniraft/raft.h>
#include <miniraft/snapshot_codec.h>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <iterator>
#include <tuple>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <csignal>
#include <unistd.h>
extern "C" {
#include <cmocka.h>
}

namespace {
namespace fs = std::filesystem;
using TStage = TDurableState::EStage;
struct TFiles {
    fs::path Directory;
    std::string Name;
    TFiles() {
        static unsigned next = 0;
        Directory = fs::temp_directory_path() / ("miniraft-durable-" +
            std::to_string(getpid()) + "-" + std::to_string(next++));
        assert_true(fs::create_directory(Directory));
        Name = (Directory / "raft").string();
    }
    ~TFiles() { std::error_code ignored; fs::remove_all(Directory, ignored); }
    fs::path Image() const { return Name + ".durable.1"; }
    void Seed();
};
void Put(std::string& bytes, uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) bytes.push_back(char(value >> (8 * i)));
}
uint32_t Crc32(std::string_view bytes) {
    uint32_t crc = UINT32_MAX;
    for (unsigned char byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
std::string LegacyImage(std::string_view magic, bool hasSnapshot, bool configured) {
    std::string payload;
    Put(payload, 3, 8); // current term
    Put(payload, 1, 4); // vote
    Put(payload, 1, 8); // retained entry count
    if (configured) {
        Put(payload, 2, 4); Put(payload, 1, 4); Put(payload, 2, 4);
    }
    if (hasSnapshot) {
        const auto image = EncodeSnapshot(TSnapshot{1, 2, {1, 2}, "legacy snapshot"});
        Put(payload, image.size(), 8); payload.append(image);
    }
    Put(payload, 3, 8); Put(payload, 0, 8); Put(payload, 4, 4); payload.append("tail");
    std::string checksumInput;
    if (magic != "MRFTD001") checksumInput.append(magic);
    checksumInput.append(payload);
    std::string bytes(magic);
    Put(bytes, payload.size(), 8); Put(bytes, Crc32(checksumInput), 4); bytes.append(payload);
    return bytes;
}
TMessageHolder<TLogEntry> Entry(uint64_t term, const std::string& data) {
    auto entry = NewHoldedMessage<TLogEntry>(sizeof(TLogEntry) + data.size());
    entry->Term = term;
    memcpy(entry->Data, data.data(), data.size());
    return entry;
}
std::string Data(TDurableState& state, int64_t index);

void test_v1_v2_v3_images_recover_conservatively_and_upgrade_on_publish(void**) {
    for (const auto [magic, hasSnapshot, configured] : {
        std::tuple<std::string_view, bool, bool>{"MRFTD001", false, false},
        {"MRFTD002", true, false},
        {"MRFTD003", true, true},
    }) {
        TFiles files;
        const auto legacy = LegacyImage(magic, hasSnapshot, configured);
        { std::ofstream output(files.Image(), std::ios::binary); output.write(legacy.data(), legacy.size()); }
        {
            TDurableState state(files.Name, 1);
            assert_int_equal(state.CurrentTerm, 3);
            assert_int_equal(state.SnapshotIndex, hasSnapshot ? 1 : 0);
            assert_int_equal(state.CommitIndex, state.SnapshotIndex);
            assert_int_equal(state.LastLogIndex, hasSnapshot ? 2 : 1);
            if (!configured) state.BindVotersFromTrustedBaseline({1, 2});
            assert_true((state.GetBoundVoters() == std::vector<uint32_t>{1, 2}));
            assert_true(state.SupportsCommitIndexPersistence());
            if (hasSnapshot) assert_true(state.GetSnapshot()->Data == "legacy snapshot");
            assert_string_equal(Data(state, state.LastLogIndex).c_str(), "tail");
            state.Commit();
        }
        std::ifstream upgraded(files.Image(), std::ios::binary);
        std::string published(8, '\0'); upgraded.read(published.data(), published.size());
        assert_true(published == "MRFTD004");
    }
}
void TFiles::Seed() {
    TDurableState state(Name, 1);
    state.CurrentTerm = 3;
    state.VotedFor = 2;
    state.Append(Entry(1, "first"));
    state.Append(Entry(2, "second"));
}
std::string Data(TDurableState& state, int64_t index) {
    auto entry = state.Get(index);
    return std::string(entry->Data, entry->Len - sizeof(TLogEntry));
}
enum class EOperation { Append, Truncate, Vote, Replace, CommitIndex };
void Operate(TDurableState& state, EOperation operation) {
    if (operation == EOperation::Truncate) state.RemoveLast();
    else if (operation == EOperation::CommitIndex) {
        state.CommitIndex = 1;
        state.Commit();
    }
    else if (operation == EOperation::Vote) {
        state.CurrentTerm = 4;
        state.VotedFor = 3;
        state.Commit();
    } else state.Append(Entry(3, "replacement"));
}
void Check(TDurableState& state, EOperation operation, bool updated) {
    assert_int_equal(state.CurrentTerm, updated && operation == EOperation::Vote ? 4 : 3);
    assert_int_equal(state.VotedFor, updated && operation == EOperation::Vote ? 3 : 2);
    const auto oldCount = operation == EOperation::Replace ? 1 : 2;
    const auto count = !updated ? oldCount : operation == EOperation::Truncate ? 1 :
        operation == EOperation::Vote || operation == EOperation::CommitIndex ? 2 : oldCount + 1;
    assert_int_equal(state.LastLogIndex, count);
    assert_int_equal(state.CommitIndex, updated && operation == EOperation::CommitIndex ? 1 : 0);
    assert_string_equal(Data(state, 1).c_str(), "first");
    if (count >= 2) {
        const bool replaced = updated && operation == EOperation::Replace;
        assert_string_equal(Data(state, 2).c_str(), replaced ? "replacement" : "second");
        assert_int_equal(state.Get(2)->Term, replaced ? 3 : 2);
    }
    if (count == 3) assert_string_equal(Data(state, 3).c_str(), "replacement");
    assert_int_equal(state.LastLogTerm, state.Get(count)->Term);
}
void Wait(pid_t child, int expected) {
    int status;
    assert_int_equal(waitpid(child, &status, 0), child);
    assert_true(WIFEXITED(status));
    assert_int_equal(WEXITSTATUS(status), expected);
}

void test_crash_at_every_publication_boundary(void**) {
    for (auto operation : {EOperation::Append, EOperation::Truncate, EOperation::Vote, EOperation::Replace, EOperation::CommitIndex}) {
        for (auto stage : {TStage::TempCreated, TStage::Written, TStage::FileSynced,
                           TStage::Renamed, TStage::DirectorySynced}) {
            TFiles files;
            files.Seed();
            const auto child = fork();
            assert_true(child >= 0);
            if (child == 0) {
                try {
                    bool armed = false;
                    TDurableState state(files.Name, 1, [&](auto point) {
                        if (armed && point == stage) _exit(90);
                    });
                    if (operation == EOperation::Replace) state.RemoveLast();
                    armed = true;
                    Operate(state, operation);
                } catch (...) { _exit(2); }
                _exit(3);
            }
            Wait(child, 90);
            TDurableState recovered(files.Name, 1);
            Check(recovered, operation, stage >= TStage::Renamed);
        }
    }
}

void test_acknowledged_updates_survive_immediate_exit(void**) {
    for (auto operation : {EOperation::Append, EOperation::Truncate, EOperation::Vote, EOperation::Replace, EOperation::CommitIndex}) {
        TFiles files;
        files.Seed();
        const auto child = fork();
        assert_true(child >= 0);
        if (child == 0) {
            try {
                TDurableState state(files.Name, 1);
                if (operation == EOperation::Replace) state.RemoveLast();
                Operate(state, operation);
                _exit(0); // No destructors or stream flushing after acknowledgement.
            } catch (...) { _exit(2); }
        }
        Wait(child, 0);
        TDurableState recovered(files.Name, 1);
        Check(recovered, operation, true);
    }
}

struct TEndpoint : INode {
    std::vector<TMessageHolder<TMessage>> Sent;
    std::vector<std::string>* Events = nullptr;
    void Send(TMessageHolder<TMessage> message) override {
        Sent.push_back(std::move(message));
        if (Events) Events->push_back("ack");
    }
    void Drain() override {}
};

void test_follower_persists_commit_before_ack_and_recovers_it(void**) {
    TFiles files;
    std::vector<std::string> events;
    auto peer = std::make_shared<TEndpoint>();
    peer->Events = &events;
    {
        auto state = std::make_shared<TDurableState>(files.Name, 1, [&](auto stage) {
            if (stage == TStage::DirectorySynced) events.push_back("durable");
        });
        TRaft raft(state, 1, {{2, peer}});
        events.clear(); // Ignore initial image and voter-binding publications.
        auto request = NewHoldedMessage(TMessageEx{.Src = 2, .Dst = 1, .Term = 1},
            TAppendEntriesRequest{.PrevLogIndex = 0, .PrevLogTerm = 0, .LeaderCommit = 1,
                                  .LeaderId = 2, .Nentries = 1});
        request.InitPayload(1);
        request.Payload[0] = Entry(1, "committed");
        raft.Process(ITimeSource::Time{}, request);
        assert_int_equal(state->CommitIndex, 1);
        assert_int_equal(peer->Sent.size(), 1);
        assert_true(peer->Sent.back().Cast<TAppendEntriesResponse>()->Success);
        assert_true((events == std::vector<std::string>{"durable", "durable", "ack"}));
    }
    TDurableState recovered(files.Name, 1);
    assert_int_equal(recovered.CommitIndex, 1);
    assert_int_equal(recovered.LastLogIndex, 1);
    bool protectedEntry = false;
    try { recovered.RemoveLast(); }
    catch (const std::logic_error&) { protectedEntry = true; }
    assert_true(protectedEntry);
    assert_int_equal(recovered.LastLogIndex, 1);
}

void test_publication_order(void**) {
    TFiles files;
    files.Seed();
    std::vector<TStage> stages;
    TDurableState state(files.Name, 1, [&](auto stage) { stages.push_back(stage); });
    state.CurrentTerm = 4;
    state.Commit();
    const std::vector<TStage> expected{TStage::TempCreated, TStage::Written, TStage::FileSynced,
                                      TStage::Renamed, TStage::DirectorySynced};
    assert_true(stages == expected);
}

void test_live_publication_failure_is_fatal(void**) {
    TFiles files;
    files.Seed();
    const auto child = fork();
    assert_true(child >= 0);
    if (child == 0) {
        std::set_terminate([] { _exit(73); });
        try {
            TDurableState state(files.Name, 1, [](auto stage) {
                if (stage == TStage::FileSynced) throw std::runtime_error("injected failure");
            });
            state.CurrentTerm = 4;
            state.Commit();
        } catch (...) { _exit(74); }
        _exit(75);
    }
    Wait(child, 73);
    TDurableState recovered(files.Name, 1);
    assert_int_equal(recovered.CurrentTerm, 3);
    assert_int_equal(recovered.VotedFor, 2);
}

void test_write_error_is_fatal_and_preserves_image(void**) {
    TFiles files;
    files.Seed();
    const auto child = fork();
    assert_true(child >= 0);
    if (child == 0) {
        std::set_terminate([] { _exit(73); });
        try {
            TDurableState state(files.Name, 1);
            struct rlimit limit;
            if (getrlimit(RLIMIT_FSIZE, &limit) != 0) _exit(76);
            limit.rlim_cur = 0;
            if (setrlimit(RLIMIT_FSIZE, &limit) != 0) _exit(76);
            std::signal(SIGXFSZ, SIG_IGN);
            state.Append(Entry(3, "must not be acknowledged"));
        } catch (...) { _exit(74); }
        _exit(75);
    }
    Wait(child, 73);
    TDurableState recovered(files.Name, 1);
    Check(recovered, EOperation::Append, false);
}

void test_interrupted_initialization_is_not_reset(void**) {
    for (auto stage : {TStage::TempCreated, TStage::Written, TStage::FileSynced,
                       TStage::Renamed, TStage::DirectorySynced}) {
        TFiles files;
        const auto child = fork();
        assert_true(child >= 0);
        if (child == 0) {
            try {
                TDurableState state(files.Name, 1, [&](auto point) {
                    if (point == stage) _exit(90);
                });
            } catch (...) { _exit(2); }
            _exit(3);
        }
        Wait(child, 90);
        bool rejected = false;
        try {
            TDurableState recovered(files.Name, 1);
            assert_int_equal(recovered.CurrentTerm, 1);
            assert_int_equal(recovered.VotedFor, 0);
            assert_int_equal(recovered.LastLogIndex, 0);
        } catch (const std::runtime_error&) { rejected = true; }
        assert_int_equal(rejected, stage < TStage::Renamed);
    }
}

void Reject(const TFiles& files) {
    bool rejected = false;
    try { TDurableState state(files.Name, 1); }
    catch (const std::runtime_error&) { rejected = true; }
    assert_true(rejected);
}
void test_corrupt_images_are_rejected(void**) {
    for (const auto offset : {0, 8, 16, 20, 30, 45}) {
        TFiles files;
        files.Seed();
        std::fstream file(files.Image(), std::ios::in | std::ios::out | std::ios::binary);
        file.seekg(offset);
        char byte;
        file.read(&byte, 1);
        byte ^= 1;
        file.seekp(offset);
        file.write(&byte, 1);
        file.close();
        Reject(files);
    }
    for (const auto size : {0, 7, 19, 21, 40}) {
        TFiles files;
        files.Seed();
        fs::resize_file(files.Image(), size);
        Reject(files);
    }
}
void test_single_writer_lock(void**) {
    TFiles files;
    files.Seed();
    {
        TDurableState writer(files.Name, 1);
        Reject(files);
    }
    TDurableState afterClose(files.Name, 1);
    assert_int_equal(afterClose.CurrentTerm, 3);
}
void test_missing_image_is_not_recreated(void**) {
    TFiles files;
    files.Seed();
    fs::remove(files.Image());
    Reject(files);
    assert_false(fs::exists(files.Image()));
}
void test_legacy_store_is_not_silently_migrated(void**) {
    TFiles files;
    { TDiskState legacy(files.Name, 1); }
    Reject(files);
    assert_false(fs::exists(files.Image()));
}
void test_get_and_append_do_not_share_mutable_entries(void**) {
    TFiles files;
    {
        TDurableState state(files.Name, 1);
        auto entry = Entry(1, "data");
        state.Append(entry);
        entry->Term = 100;
        auto result = state.Get(1);
        assert_int_equal(result->Term, 1);
        result->Term = 99;
        assert_int_equal(state.Get(1)->Term, 1);
        state.Commit();
    }
    TDurableState state(files.Name, 1);
    assert_int_equal(state.Get(1)->Term, 1);
}
void test_raft_election_and_application_replay_after_restart(void**) {
    TFiles files;
    uint64_t term = 0, index = 0;
    for (int restart = 0; restart < 2; ++restart) {
        auto state = std::make_shared<TDurableState>(files.Name, 1);
        auto raft = std::make_shared<TRaft>(state, 1, TNodeDict{});
        raft->ProcessTimeout(ITimeSource::Time{});
        assert_int_equal(raft->CurrentStateName(), EState::LEADER);
        assert_true(state->CurrentTerm > term);
        term = state->CurrentTerm;
        if (restart == 0) {
            index = raft->Append(Entry(term, "application-command"));
            raft->ProcessTimeout(ITimeSource::Time{});
        }
        assert_true(raft->GetVolatileState()->CommitIndex >= index);
        assert_string_equal(Data(*state, index).c_str(), "application-command");
        auto rsm = std::make_shared<TDummyRsm>();
        TRequestProcessor processor(raft, rsm, {});
        processor.ProcessCommitted();
        assert_int_equal(rsm->LastAppliedIndex, state->LastLogIndex);
    }
}
}

int main() {
    const CMUnitTest tests[] = {
        cmocka_unit_test(test_crash_at_every_publication_boundary),
        cmocka_unit_test(test_acknowledged_updates_survive_immediate_exit),
        cmocka_unit_test(test_follower_persists_commit_before_ack_and_recovers_it),
        cmocka_unit_test(test_publication_order),
        cmocka_unit_test(test_v1_v2_v3_images_recover_conservatively_and_upgrade_on_publish),
        cmocka_unit_test(test_live_publication_failure_is_fatal),
        cmocka_unit_test(test_write_error_is_fatal_and_preserves_image),
        cmocka_unit_test(test_interrupted_initialization_is_not_reset),
        cmocka_unit_test(test_corrupt_images_are_rejected),
        cmocka_unit_test(test_single_writer_lock),
        cmocka_unit_test(test_missing_image_is_not_recreated),
        cmocka_unit_test(test_legacy_store_is_not_silently_migrated),
        cmocka_unit_test(test_get_and_append_do_not_share_mutable_entries),
        cmocka_unit_test(test_raft_election_and_application_replay_after_restart),
    };
    return cmocka_run_group_tests(tests, nullptr, nullptr);
}
