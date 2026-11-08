#include "persist.h"
#include <filesystem>
#include <stdexcept>
#include <exception>
#include <iostream>

TDiskState::TDiskState(const std::string& name, uint32_t id)
{
    const auto suffix = "." + std::to_string(id);
    const auto entriesPath = name + ".entries" + suffix;
    const auto indexPath = name + ".index" + suffix;
    const auto statePath = name + ".state" + suffix;
    const int existing = std::filesystem::exists(entriesPath) +
        std::filesystem::exists(indexPath) + std::filesystem::exists(statePath);
    // Never turn a partially missing store into a fresh Raft voter.
    if (existing != 0 && existing != 3) {
        throw std::runtime_error("Incomplete Raft storage: " + name);
    }
    const bool fresh = existing == 0;
    Entries = Open(entriesPath, fresh);
    Index = Open(indexPath, fresh);
    State = Open(statePath, fresh);
    if (fresh) {
        Commit();
        Recovering = false;
        return;
    }

    State.seekg(0, std::ios::end);
    const auto size = State.tellg();
    if (size != std::streamoff(sizeof(LastLogIndex) + sizeof(CurrentTerm) + sizeof(VotedFor))) {
        throw std::runtime_error("Invalid Raft metadata size: " + statePath);
    }
    State.seekg(0);
    if (!State.read(reinterpret_cast<char*>(&LastLogIndex), sizeof(LastLogIndex)) ||
        !State.read(reinterpret_cast<char*>(&CurrentTerm), sizeof(CurrentTerm)) ||
        !State.read(reinterpret_cast<char*>(&VotedFor), sizeof(VotedFor)) || CurrentTerm == 0) {
        throw std::runtime_error("Invalid Raft metadata: " + statePath);
    }

    Index.seekg(0, std::ios::end);
    const auto indexSize = Index.tellg();
    if (indexSize < 0 || LastLogIndex > uint64_t(indexSize) / sizeof(uint64_t)) {
        throw std::runtime_error("Truncated Raft index: " + indexPath);
    }
    // Validate the entire selected prefix, not only its last entry. Trailing
    // bytes can be left by logical truncation and are deliberately ignored.
    uint64_t expectedOffset = 0;
    for (uint64_t i = 1; i <= LastLogIndex; ++i) {
        uint64_t offset;
        Index.seekg((i - 1) * sizeof(offset));
        if (!Index.read(reinterpret_cast<char*>(&offset), sizeof(offset)) || offset != expectedOffset) {
            throw std::runtime_error("Invalid Raft log offset: " + indexPath);
        }
        auto entry = Get(i);
        expectedOffset += entry->Len;
        LastLogTerm = entry->Term;
    }
    Recovering = false;
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
    if (Recovering) throw std::runtime_error(message);
    // The network layer catches ordinary exceptions and keeps the node alive.
    // Continuing after a live storage failure could acknowledge unpersisted
    // votes or logs. Fail-stop instead; startup validation remains catchable.
    std::cerr << "Fatal Raft storage error: " << message << '\n';
    std::terminate();
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
    if (LastLogIndex > 0) {
        const auto previousTerm = LastLogIndex == 1 ? 0 : Get(LastLogIndex - 1)->Term;
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
    Index.seekp(LastLogIndex * sizeof(offset));
    if (!Index.write(reinterpret_cast<const char*>(&offset), sizeof(offset))) {
        Fail("Cannot write Raft log index");
    }
    LastLogTerm = entry->Term;
    ++LastLogIndex;
    Commit();
}

TMessageHolder<TLogEntry> TDiskState::Get(int64_t index) const
{
    if (index > LastLogIndex || index < 1) return {};
    uint64_t offset;
    Index.seekg((index - 1) * sizeof(offset));
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
    State.flush();
    if (!State) Fail("Cannot flush Raft metadata");
}
