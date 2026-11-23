#include "persist.h"
#include <charconv>
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <iostream>

namespace {
// Metadata without a snapshot keeps the original size and layout.
constexpr std::streamoff LegacyStateSize = sizeof(uint64_t) * 2 + sizeof(uint32_t);
constexpr std::streamoff SnapshotStateSize = LegacyStateSize + sizeof(uint64_t) * 2;
}

TDiskState::TDiskState(const std::string& name, uint32_t id)
    : Name(name)
    , Id(id)
{
    const auto statePath = Path("state", 0);
    const auto files = Files();
    const bool existing = std::filesystem::exists(statePath);
    if (!existing && files.empty()) {
        Entries = Open(Path("entries", 0), true);
        Index = Open(Path("index", 0), true);
        State = Open(statePath, true);
        Commit();
        return;
    }
    // Never turn a partially missing store into a fresh Raft voter.
    if (!existing) {
        throw std::runtime_error("Incomplete Raft storage: " + name);
    }
    State = Open(statePath, false);

    State.seekg(0, std::ios::end);
    const auto size = State.tellg();
    if (size != LegacyStateSize && size != SnapshotStateSize) {
        throw std::runtime_error("Invalid Raft metadata size: " + statePath);
    }
    State.seekg(0);
    if (!State.read(reinterpret_cast<char*>(&LastLogIndex), sizeof(LastLogIndex)) ||
        !State.read(reinterpret_cast<char*>(&CurrentTerm), sizeof(CurrentTerm)) ||
        !State.read(reinterpret_cast<char*>(&VotedFor), sizeof(VotedFor)) || CurrentTerm == 0) {
        throw std::runtime_error("Invalid Raft metadata: " + statePath);
    }
    if (size == SnapshotStateSize && (
        !State.read(reinterpret_cast<char*>(&SnapshotIndex), sizeof(SnapshotIndex)) ||
        !State.read(reinterpret_cast<char*>(&SnapshotTerm), sizeof(SnapshotTerm)) ||
        !SnapshotIndex || SnapshotIndex > LastLogIndex || !SnapshotTerm || SnapshotTerm > CurrentTerm)) {
        throw std::runtime_error("Invalid Raft snapshot metadata: " + statePath);
    }

    const auto entriesPath = Path("entries", SnapshotIndex);
    const auto indexPath = Path("index", SnapshotIndex);
    if (!std::filesystem::exists(entriesPath) || !std::filesystem::exists(indexPath) ||
        (SnapshotIndex && !std::filesystem::exists(Path("snapshot", SnapshotIndex)))) {
        throw std::runtime_error("Incomplete Raft storage: " + name);
    }
    Entries = Open(entriesPath, false);
    Index = Open(indexPath, false);

    Index.seekg(0, std::ios::end);
    const auto indexSize = Index.tellg();
    if (indexSize < 0 || LastLogIndex - SnapshotIndex > uint64_t(indexSize) / sizeof(uint64_t)) {
        throw std::runtime_error("Truncated Raft index: " + indexPath);
    }
    // Validate the entire selected prefix, not only its last entry. Trailing
    // bytes can be left by logical truncation and are deliberately ignored.
    uint64_t expectedOffset = 0;
    LastLogTerm = SnapshotTerm;
    for (uint64_t i = SnapshotIndex + 1; i <= LastLogIndex; ++i) {
        uint64_t offset;
        Index.seekg((i - SnapshotIndex - 1) * sizeof(offset));
        if (!Index.read(reinterpret_cast<char*>(&offset), sizeof(offset)) || offset != expectedOffset) {
            throw std::runtime_error("Invalid Raft log offset: " + indexPath);
        }
        auto entry = Get(i);
        if (entry->Term < LastLogTerm) {
            throw std::runtime_error("Decreasing Raft log terms: " + entriesPath);
        }
        expectedOffset += entry->Len;
        LastLogTerm = entry->Term;
    }
    GetSnapshot(); // validates the image against the metadata

    // Left by a snapshot publication that a crash interrupted before or after its commit.
    for (const auto& [path, snapshotIndex] : files) {
        if (snapshotIndex != SnapshotIndex) {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    }
}

std::string TDiskState::Path(const std::string& kind, uint64_t snapshotIndex) const
{
    auto path = Name + "." + kind + "." + std::to_string(Id);
    if (snapshotIndex) {
        path += "." + std::to_string(snapshotIndex);
    }
    return path;
}

std::vector<std::pair<std::string, uint64_t>> TDiskState::Files() const
{
    // Every log and snapshot file of this store with the snapshot index in its name.
    namespace fs = std::filesystem;
    const fs::path base(Name);
    const auto directory = base.has_parent_path() ? base.parent_path() : fs::path(".");
    std::vector<std::pair<std::string, uint64_t>> files;
    std::error_code error;
    for (const auto& file : fs::directory_iterator(directory, error)) {
        const auto filename = file.path().filename().string();
        for (const std::string kind : {"entries", "index", "snapshot"}) {
            const auto prefix = base.filename().string() + "." + kind + "." + std::to_string(Id);
            if (filename.compare(0, prefix.size(), prefix) != 0) {
                continue;
            }
            const std::string_view rest = std::string_view(filename).substr(prefix.size());
            uint64_t snapshotIndex = 0;
            if (!rest.empty() &&
                std::from_chars(rest.data() + 1, rest.data() + rest.size(), snapshotIndex).ec != std::errc{}) {
                continue;
            }
            // Only the names this store writes: not "raft.entries.1.07", for example.
            if (fs::path(Path(kind, snapshotIndex)).filename() != filename) {
                continue;
            }
            files.emplace_back(file.path().string(), snapshotIndex);
        }
    }
    return files;
}

std::fstream TDiskState::Open(const std::string& name, bool create)
{
    if (create) {
        std::ofstream file(name, std::ios::binary);
        if (!file) throw std::runtime_error("Cannot create file: " + name);
    }
    std::fstream file(name, std::ios::in | std::ios::out | std::ios::binary);
    // An open failure must never fall back to truncating an existing file.
    if (!file) throw std::runtime_error("Cannot open file: " + name);
    return file;
}

[[noreturn]] void TDiskState::Fail(const char* message) const
{
    // Report to the embedder; whether a live storage error stops the node is its policy.
    throw std::runtime_error(message);
}

TMessageHolder<TMessage> TDiskState::Read() const
{
    const auto start = Entries.tellg();
    Entries.seekg(0, std::ios::end);
    const auto end = Entries.tellg();
    if (start < 0 || end < start || end - start < std::streamoff(sizeof(TLogEntry))) {
        Fail("Truncated Raft log entry");
    }
    Entries.seekg(start);
    uint32_t type, len;
    if (!Entries.read(reinterpret_cast<char*>(&type), sizeof(type)) ||
        !Entries.read(reinterpret_cast<char*>(&len), sizeof(len)) ||
        type != uint32_t(EMessageType::LOG_ENTRY) || len < sizeof(TLogEntry) ||
        uint64_t(len) > uint64_t(end - start)) {
        Fail("Invalid Raft log entry header");
    }
    // Bound the allocation and subtraction by the validated file extent.
    auto message = NewHoldedMessage<TMessage>(type, len);
    if (!Entries.read(reinterpret_cast<char*>(message->Value), len - sizeof(TMessage))) {
        Fail("Cannot read Raft log entry");
    }
    const auto term = message.Cast<TLogEntry>()->Term;
    if (!term || term > CurrentTerm) {
        Fail("Raft log term outside current term range");
    }
    return message;
}

void TDiskState::Write(TMessageHolder<TMessage> message)
{
    if (!Entries.write(reinterpret_cast<const char*>(message.Mes), message->Len)) {
        Fail("Cannot write Raft log entry");
    }
}

void TDiskState::RemoveLast()
{
    if (LastLogIndex > SnapshotIndex) {
        const auto previousTerm = LastLogIndex - 1 == SnapshotIndex ? SnapshotTerm : Get(LastLogIndex - 1)->Term;
        --LastLogIndex;
        LastLogTerm = previousTerm;
        Commit();
    }
}

void TDiskState::Append(TMessageHolder<TLogEntry> entry)
{
    uint64_t offset = 0;
    if (Get(LastLogIndex)) {
        offset = Entries.tellg();
    }
    // In particular, after truncation to zero the stream may still point into
    // the old suffix. Always position the output explicitly.
    Entries.seekp(offset);
    Write(entry);
    Index.seekp((LastLogIndex - SnapshotIndex) * sizeof(offset));
    if (!Index.write(reinterpret_cast<const char*>(&offset), sizeof(offset))) {
        Fail("Cannot write Raft log index");
    }
    LastLogTerm = entry->Term;
    ++LastLogIndex;
    Commit();
}

TMessageHolder<TLogEntry> TDiskState::Get(int64_t index) const
{
    if (index > LastLogIndex || index <= int64_t(SnapshotIndex)) return {};
    uint64_t offset;
    Index.seekg((index - SnapshotIndex - 1) * sizeof(offset));
    if (!Index.read(reinterpret_cast<char*>(&offset), sizeof(offset))) {
        Fail("Cannot read Raft log index");
    }
    Entries.seekg(0, std::ios::end);
    const auto end = Entries.tellg();
    if (end < 0 || offset > uint64_t(end)) {
        Fail("Raft log offset outside file");
    }
    Entries.seekg(offset);
    return Read().Cast<TLogEntry>();
}

void TDiskState::Commit()
{
    // Publish buffered log data before metadata. Stream flushing alone is not
    // a power-loss durability protocol; fsync/atomic publication remain TODO.
    Entries.flush();
    Index.flush();
    if (!Entries || !Index) Fail("Cannot flush Raft log");
    State.seekp(0);
    if (!State.write(reinterpret_cast<const char*>(&LastLogIndex), sizeof(LastLogIndex)) ||
        !State.write(reinterpret_cast<const char*>(&CurrentTerm), sizeof(CurrentTerm)) ||
        !State.write(reinterpret_cast<const char*>(&VotedFor), sizeof(VotedFor))) {
        Fail("Cannot write Raft metadata");
    }
    if (SnapshotIndex && (
        !State.write(reinterpret_cast<const char*>(&SnapshotIndex), sizeof(SnapshotIndex)) ||
        !State.write(reinterpret_cast<const char*>(&SnapshotTerm), sizeof(SnapshotTerm)))) {
        Fail("Cannot write Raft metadata");
    }
    State.flush();
    if (!State) Fail("Cannot flush Raft metadata");
}

TMessageHolder<TInstallSnapshotRequest> TDiskState::GetSnapshot() const
{
    if (!SnapshotIndex) {
        return {};
    }
    // The image is stored as its header frame followed by its record frames.
    std::ifstream file(Path("snapshot", SnapshotIndex), std::ios::binary | std::ios::ate);
    const auto size = file.tellg();
    file.seekg(0);
    if (!file || size < 0) {
        Fail("Cannot open Raft snapshot");
    }
    uint64_t remaining = size;
    auto read = [&](uint32_t type, bool header) {
        uint32_t frame[2];
        if (remaining < sizeof(frame) || !file.read(reinterpret_cast<char*>(frame), sizeof(frame)) ||
            frame[0] != type || frame[1] > remaining ||
            (header ? frame[1] != sizeof(TInstallSnapshotRequest) : frame[1] < sizeof(TLogEntry))) {
            Fail("Invalid Raft snapshot frame");
        }
        remaining -= frame[1];
        auto message = NewHoldedMessage<TMessage>(frame[0], frame[1]);
        if (!file.read(message->Value, frame[1] - sizeof(TMessage))) {
            Fail("Cannot read Raft snapshot");
        }
        return message;
    };
    auto snapshot = read(uint32_t(EMessageType::INSTALL_SNAPSHOT_REQUEST), true).Cast<TInstallSnapshotRequest>();
    if (snapshot->LastIncludedIndex != SnapshotIndex || snapshot->LastIncludedTerm != SnapshotTerm ||
        uint64_t(snapshot->Nentries) * sizeof(TLogEntry) > remaining) {
        Fail("Raft snapshot does not match metadata");
    }
    snapshot.InitPayload(snapshot->Nentries);
    for (uint32_t i = 0; i < snapshot->Nentries; ++i) {
        snapshot.Payload[i] = read(uint32_t(EMessageType::LOG_ENTRY), false);
    }
    if (remaining) {
        Fail("Trailing bytes in Raft snapshot");
    }
    return snapshot;
}

void TDiskState::InstallSnapshot(TMessageHolder<TInstallSnapshotRequest> snapshot)
{
    const auto index = snapshot->LastIncludedIndex;
    const auto term = snapshot->LastIncludedTerm;
    if (index <= SnapshotIndex || !term || term > CurrentTerm) {
        throw std::invalid_argument("Invalid Raft snapshot boundary");
    }
    const bool retain = index <= LastLogIndex && LogTerm(index) == term;
    // Files named by the new index become current only when Commit publishes it.
    auto image = Open(Path("snapshot", index), true);
    const auto header = NewHoldedMessage(TMessageEx{}, TInstallSnapshotRequest{
        .LastIncludedIndex = index,
        .LastIncludedTerm = term,
        .Nentries = snapshot.PayloadSize,
        .Done = 1,
    });
    image.write(reinterpret_cast<const char*>(header.Mes), header->Len);
    for (uint32_t i = 0; i < snapshot.PayloadSize; ++i) {
        const auto& record = snapshot.Payload[i];
        image.write(reinterpret_cast<const char*>(record.Mes), record->Len);
    }
    image.flush();
    if (!image) {
        Fail("Cannot write Raft snapshot");
    }
    auto entries = Open(Path("entries", index), true);
    auto offsets = Open(Path("index", index), true);
    uint64_t offset = 0;
    for (auto i = index + 1; retain && i <= LastLogIndex; ++i) {
        auto entry = Get(i);
        entries.write(reinterpret_cast<const char*>(entry.Mes), entry->Len);
        offsets.write(reinterpret_cast<const char*>(&offset), sizeof(offset));
        offset += entry->Len;
    }
    entries.flush();
    offsets.flush();
    if (!entries || !offsets) {
        Fail("Cannot write Raft log");
    }

    const auto previous = SnapshotIndex;
    Entries = std::move(entries);
    Index = std::move(offsets);
    if (!retain) {
        LastLogIndex = index;
        LastLogTerm = term;
    }
    SnapshotIndex = index;
    SnapshotTerm = term;
    Commit();
    // An fsync of the new files and the metadata is desirable here: without it a power
    // loss after the removal below can lose both generations.
    for (const std::string kind : {"entries", "index", "snapshot"}) {
        std::error_code ignored;
        std::filesystem::remove(Path(kind, previous), ignored);
    }
}
