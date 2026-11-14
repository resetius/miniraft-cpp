#include "durable.h"
#include <array>
#include <climits>
#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <exception>
#include <iostream>
#ifdef __linux__
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <system_error>
#endif

namespace {
using TBytes = std::vector<uint8_t>;
constexpr std::array<uint8_t, 8> Magic{'M','R','F','T','D','0','0','1'};
constexpr std::array<uint8_t, 8> SnapshotMagic{'M','R','F','T','D','0','0','2'};
constexpr std::array<uint8_t, 8> ConfigurationMagic{'M','R','F','T','D','0','0','3'};
void Put(TBytes& bytes, uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) bytes.push_back(uint8_t(value >> (8 * i)));
}
uint64_t Take(const TBytes& bytes, size_t& position, unsigned width) {
    if (position > bytes.size() || width > bytes.size() - position)
        throw std::runtime_error("Truncated durable state");
    uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i) value |= uint64_t(bytes[position++]) << (8 * i);
    return value;
}
uint32_t Checksum(const TBytes& bytes, size_t start, const std::array<uint8_t, 8>* version = nullptr) {
    uint32_t crc = UINT32_MAX;
    const auto update = [&](uint8_t byte) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    };
    if (version) for (auto byte : *version) update(byte);
    for (size_t i = start; i < bytes.size(); ++i) update(bytes[i]);
    return ~crc;
}
TMessageHolder<TLogEntry> Clone(TMessageHolder<TLogEntry> entry) {
    if (!entry.HasValidExtent() || entry->Len < sizeof(TLogEntry) || entry->Type != uint32_t(EMessageType::LOG_ENTRY))
        throw std::runtime_error("Invalid durable log entry");
    auto copy = NewHoldedMessage<TLogEntry>(entry->Len);
    memcpy(copy.Mes, entry.Mes, entry->Len);
    return copy;
}
#ifdef __linux__
struct TFd {
    int Value = -1;
    TFd() = default;
    explicit TFd(int fd) : Value(fd) {}
    ~TFd() { if (Value >= 0) close(Value); }
    TFd(const TFd&) = delete;
    TFd& operator=(const TFd&) = delete;
};
void Require(bool ok, const char* operation) {
    if (!ok) throw std::system_error(errno, std::generic_category(), operation);
}
void Sync(int fd) {
    int result;
    do { result = fsync(fd); } while (result < 0 && errno == EINTR);
    Require(result == 0, "fsync durable state");
}
void WriteAll(int fd, const TBytes& bytes) {
    size_t done = 0;
    while (done < bytes.size()) {
        auto n = write(fd, bytes.data() + done, bytes.size() - done);
        if (n < 0 && errno == EINTR) continue;
        Require(n > 0, "write durable state");
        done += size_t(n);
    }
}
TBytes ReadAll(int fd) {
    struct stat info;
    Require(fstat(fd, &info) == 0, "stat durable state");
    if (!S_ISREG(info.st_mode) || info.st_size < 20 || uint64_t(info.st_size) > SIZE_MAX)
        throw std::runtime_error("Invalid durable state file size/type");
    TBytes bytes(size_t(info.st_size));
    size_t done = 0;
    while (done < bytes.size()) {
        auto n = read(fd, bytes.data() + done, bytes.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n == 0) throw std::runtime_error("Truncated durable state file");
        Require(n > 0, "read durable state");
        done += size_t(n);
    }
    return bytes;
}
#endif
}

struct TDurableState::TImpl {
    std::vector<TMessageHolder<TLogEntry>> Log;
    std::shared_ptr<const TSnapshot> Snapshot;
    std::vector<uint32_t> Voters;
    bool Recovered = false;
    bool RaftStarted = false;
    THook Hook;
    std::string File, Temp;
#ifdef __linux__
    TFd Directory, Lock;
#endif
    void Stage(EStage stage) { if (Hook) Hook(stage); }
};

TDurableState::TDurableState(const std::string& name, uint32_t id, THook hook)
    : Impl(std::make_unique<TImpl>())
{
#ifndef __linux__
    throw std::runtime_error("TDurableState currently requires Linux");
#else
    const auto base = std::filesystem::path(name);
    auto directory = base.parent_path();
    if (directory.empty()) directory = ".";
    Impl->File = base.filename().string() + ".durable." + std::to_string(id);
    Impl->Temp = Impl->File + ".tmp";
    Impl->Hook = std::move(hook);
    Impl->Directory.Value = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    Require(Impl->Directory.Value >= 0, "open durable directory");
    // Never interpret a legacy store as a fresh node. Migration must be explicit.
    for (const auto* kind : {"entries", "index", "state"}) {
        if (std::filesystem::exists(name + "." + kind + "." + std::to_string(id)))
            throw std::runtime_error("Legacy storage exists; durable migration is not automatic");
    }
    const auto lockName = Impl->File + ".lock";
    Impl->Lock.Value = openat(Impl->Directory.Value, lockName.c_str(),
                            O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    const bool newLock = Impl->Lock.Value >= 0;
    if (!newLock && errno == EEXIST)
        Impl->Lock.Value = openat(Impl->Directory.Value, lockName.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    Require(Impl->Lock.Value >= 0, "open durable lock");
    Require(flock(Impl->Lock.Value, LOCK_EX | LOCK_NB) == 0, "lock durable state");
    TFd file(openat(Impl->Directory.Value, Impl->File.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (file.Value < 0) {
        Require(errno == ENOENT, "open durable state");
        if (!newLock) throw std::runtime_error("Published durable state is missing");
        Publish();
        return;
    }
    const auto bytes = ReadAll(file.Value);
    const bool configured = std::equal(ConfigurationMagic.begin(), ConfigurationMagic.end(), bytes.begin());
    const bool compacted = std::equal(SnapshotMagic.begin(), SnapshotMagic.end(), bytes.begin());
    const auto* version = configured ? &ConfigurationMagic : compacted ? &SnapshotMagic : nullptr;
    if (!version && !std::equal(Magic.begin(), Magic.end(), bytes.begin()))
        throw std::runtime_error("Unknown durable state format");
    size_t position = Magic.size();
    const auto length = Take(bytes, position, 8);
    const auto crc = Take(bytes, position, 4);
    if (length != bytes.size() - position || crc != Checksum(bytes, position, version))
        throw std::runtime_error("Durable state length/checksum mismatch");
    CurrentTerm = Take(bytes, position, 8);
    VotedFor = Take(bytes, position, 4);
    const auto count = Take(bytes, position, 8);
    if (configured) {
        const auto voters = Take(bytes, position, 4);
        if (!voters || voters > TSnapshot::MaxVoters || voters > (bytes.size() - position) / 4)
            throw std::runtime_error("Invalid durable voter count");
        for (uint32_t i = 0; i < voters; ++i) Impl->Voters.push_back(Take(bytes, position, 4));
        try { ValidateFixedVoters(Impl->Voters); }
        catch (const std::invalid_argument&) { throw std::runtime_error("Invalid durable voter configuration"); }
    }
    if (compacted || configured) {
        const auto length = Take(bytes, position, 8);
        if (length > TSnapshot::MaxBytes || length > bytes.size() - position)
            throw std::runtime_error("Invalid embedded snapshot size");
        if (!length && compacted) throw std::runtime_error("Missing embedded snapshot");
        if (length) {
            auto snapshot = DecodeSnapshot(std::string_view(reinterpret_cast<const char*>(bytes.data() + position), size_t(length)));
            if (snapshot.HasConfigurationMetadata() && !configured)
                throw std::runtime_error("Configuration snapshot recovery requires a bound voter set");
            if (snapshot.Term > CurrentTerm) throw std::runtime_error("Future durable snapshot term");
            if (configured && snapshot.Voters != Impl->Voters) throw std::runtime_error("Durable snapshot/voter mismatch");
            position += size_t(length);
            SnapshotIndex = snapshot.Index;
            SnapshotTerm = snapshot.Term;
            LastLogTerm = SnapshotTerm;
            Impl->Snapshot = std::make_shared<const TSnapshot>(std::move(snapshot));
        }
    }
    if (!CurrentTerm || count > uint64_t(INT64_MAX) - SnapshotIndex || count > (bytes.size() - position) / 20)
        throw std::runtime_error("Invalid durable state metadata");
    for (uint64_t i = 0; i < count; ++i) {
        const auto term = Take(bytes, position, 8);
        const auto flags = Take(bytes, position, 8);
        const auto length = Take(bytes, position, 4);
        if (length > bytes.size() - position || length > UINT32_MAX - sizeof(TLogEntry) ||
            !term || term < LastLogTerm || term > CurrentTerm)
            throw std::runtime_error("Invalid durable log entry");
        auto entry = NewHoldedMessage<TLogEntry>(sizeof(TLogEntry) + length);
        entry->Term = term;
        entry->Flags = flags;
        memcpy(entry->Data, bytes.data() + position, length);
        position += length;
        Impl->Log.push_back(entry);
        LastLogTerm = term;
    }
    if (position != bytes.size()) throw std::runtime_error("Trailing durable state data");
    LastLogIndex = SnapshotIndex + count;
    // A crash after rename but before directory fsync may leave a visible new
    // image. Stabilize the selected image and its name before serving requests.
    Sync(file.Value);
    Sync(Impl->Directory.Value);
    Impl->Recovered = true;
    // A leftover temp is never a recovery candidate, even if its checksum is valid.
#endif
}

TDurableState::~TDurableState() = default;

void TDurableState::Publish()
{
#ifndef __linux__
    throw std::runtime_error("TDurableState currently requires Linux");
#else
    if (!CurrentTerm || LastLogIndex > uint64_t(INT64_MAX) || LastLogIndex != SnapshotIndex + Impl->Log.size() ||
        LastLogTerm != (Impl->Log.empty() ? SnapshotTerm : Impl->Log.back()->Term) ||
        (Impl->Snapshot ? (Impl->Snapshot->Index != SnapshotIndex || Impl->Snapshot->Term != SnapshotTerm ||
            SnapshotTerm > CurrentTerm) : (SnapshotIndex != 0 || SnapshotTerm != 0)))
        throw std::runtime_error("Invalid durable state fields");
    TBytes payload;
    Put(payload, CurrentTerm, 8);
    Put(payload, VotedFor, 4);
    Put(payload, Impl->Log.size(), 8);
    if (!Impl->Voters.empty()) {
        ValidateFixedVoters(Impl->Voters);
        if (Impl->Snapshot && Impl->Snapshot->Voters != Impl->Voters)
            throw std::runtime_error("Snapshot would change bound voters");
        Put(payload, Impl->Voters.size(), 4);
        for (auto id : Impl->Voters) Put(payload, id, 4);
    }
    if (Impl->Snapshot || !Impl->Voters.empty()) {
        const auto image = Impl->Snapshot ? EncodeSnapshot(*Impl->Snapshot) : std::string{};
        Put(payload, image.size(), 8);
        payload.insert(payload.end(), image.begin(), image.end());
    }
    uint64_t previousTerm = SnapshotTerm;
    for (auto entry : Impl->Log) {
        if (!entry->Term || entry->Term < previousTerm || entry->Term > CurrentTerm)
            throw std::runtime_error("Invalid durable log term");
        previousTerm = entry->Term;
        Put(payload, entry->Term, 8);
        Put(payload, entry->Flags, 8);
        Put(payload, entry->Len - sizeof(TLogEntry), 4);
        payload.insert(payload.end(), entry->Data, entry->Data + entry->Len - sizeof(TLogEntry));
    }
    const auto& magic = !Impl->Voters.empty() ? ConfigurationMagic : Impl->Snapshot ? SnapshotMagic : Magic;
    TBytes bytes(magic.begin(), magic.end());
    Put(bytes, payload.size(), 8);
    Put(bytes, Checksum(payload, 0, !Impl->Voters.empty() ? &ConfigurationMagic : Impl->Snapshot ? &SnapshotMagic : nullptr), 4);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    TFd temp(openat(Impl->Directory.Value, Impl->Temp.c_str(),
                    O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600));
    Require(temp.Value >= 0, "create durable temporary image");
    Impl->Stage(EStage::TempCreated);
    WriteAll(temp.Value, bytes);
    Impl->Stage(EStage::Written);
    Sync(temp.Value);
    Impl->Stage(EStage::FileSynced);
    Require(renameat(Impl->Directory.Value, Impl->Temp.c_str(),
                     Impl->Directory.Value, Impl->File.c_str()) == 0, "publish durable image");
    Impl->Stage(EStage::Renamed);
    Sync(Impl->Directory.Value);
    Impl->Stage(EStage::DirectorySynced);
#endif
}

void TDurableState::Commit()
{
    try { Publish(); }
    catch (const std::exception& error) {
        std::cerr << "Fatal durable storage error: " << error.what() << '\n';
        std::terminate();
    } catch (...) { std::terminate(); }
}

void TDurableState::Append(TMessageHolder<TLogEntry> entry)
{
    if (LastLogIndex >= uint64_t(INT64_MAX)) throw std::overflow_error("Raft log index exhausted");
    Impl->Log.push_back(Clone(entry));
    LastLogIndex = SnapshotIndex + Impl->Log.size();
    LastLogTerm = Impl->Log.back()->Term;
    Commit();
}

void TDurableState::RemoveLast()
{
    if (Impl->Log.empty()) return;
    Impl->Log.pop_back();
    LastLogIndex = SnapshotIndex + Impl->Log.size();
    LastLogTerm = Impl->Log.empty() ? SnapshotTerm : Impl->Log.back()->Term;
    Commit();
}

TMessageHolder<TLogEntry> TDurableState::Get(int64_t index) const
{
    if (index < 1 || uint64_t(index) <= SnapshotIndex || uint64_t(index) > LastLogIndex) return {};
    return Clone(Impl->Log[index - SnapshotIndex - 1]);
}

uint64_t TDurableState::RetainedLogTerm(int64_t index) const {
    if (index < 1 || uint64_t(index) <= SnapshotIndex || uint64_t(index) > LastLogIndex) return 0;
    return Impl->Log[index - SnapshotIndex - 1]->Term;
}
uint64_t TDurableState::RetainedLogFlags(int64_t index) const {
    if (index < 1 || uint64_t(index) <= SnapshotIndex || uint64_t(index) > LastLogIndex)
        throw std::out_of_range("Missing retained log entry");
    return Impl->Log[index - SnapshotIndex - 1]->Flags;
}

std::shared_ptr<const TSnapshot> TDurableState::GetSnapshot() const { return Impl->Snapshot; }

void TDurableState::InstallSnapshot(const TSnapshot& snapshot) {
    EncodeSnapshot(snapshot);
    if (snapshot.HasConfigurationMetadata() && Impl->Voters.empty())
        throw std::invalid_argument("Configuration snapshots require a bound voter set");
    if (!Impl->Voters.empty() && snapshot.Voters != Impl->Voters)
        throw std::invalid_argument("Snapshot would change bound voters");
    if (snapshot.Index <= SnapshotIndex || snapshot.Term < SnapshotTerm || snapshot.Term > CurrentTerm)
        throw std::invalid_argument("Obsolete or future durable snapshot");
    auto image = std::make_shared<const TSnapshot>(snapshot);
    std::vector<TMessageHolder<TLogEntry>> suffix;
    if (snapshot.Index <= LastLogIndex && LogTerm(snapshot.Index) == snapshot.Term)
        suffix.assign(Impl->Log.begin() + (snapshot.Index - SnapshotIndex), Impl->Log.end());
    Impl->Log.swap(suffix);
    Impl->Snapshot = std::move(image);
    SnapshotIndex = snapshot.Index;
    SnapshotTerm = snapshot.Term;
    LastLogIndex = SnapshotIndex + Impl->Log.size();
    LastLogTerm = Impl->Log.empty() ? SnapshotTerm : Impl->Log.back()->Term;
    Commit(); // Atomically publishes hard state, snapshot and the selected suffix.
}

std::vector<uint32_t> TDurableState::GetBoundVoters() const { return Impl->Voters; }

void TDurableState::MarkRaftStarted() { Impl->RaftStarted = true; }

void TDurableState::BindVoters(const std::vector<uint32_t>& voters) {
    ValidateVoterBinding(*this, Impl->Voters, voters);
    if (Impl->Voters == voters) return;
    Impl->Voters = voters;
    Commit(); // Bind before acknowledging any vote/replication traffic.
}

void TDurableState::BindVotersFromTrustedBaseline(const std::vector<uint32_t>& voters) {
    ValidateFixedVoters(voters);
    if (Impl->RaftStarted)
        throw std::invalid_argument("Trusted legacy voter migration must happen before Raft startup");
    if (!Impl->Voters.empty()) {
        if (Impl->Voters == voters) return;
        throw std::invalid_argument("Durable voter binding already exists");
    }
    if (!CurrentTerm || LastLogIndex > uint64_t(INT64_MAX) || SnapshotIndex > LastLogIndex ||
        LastLogIndex - SnapshotIndex != Impl->Log.size())
        throw std::invalid_argument("Invalid legacy state bounds");
    if (!Impl->Recovered)
        throw std::invalid_argument("Trusted legacy baseline is only for recovered state");
    if (VotedFor && !std::binary_search(voters.begin(), voters.end(), VotedFor))
        throw std::invalid_argument("Stored vote is outside the trusted voter baseline");

    if (Impl->Snapshot) {
        const auto& snapshot = *Impl->Snapshot;
        ValidateSnapshot(snapshot);
        if (snapshot.HasConfigurationMetadata() || snapshot.Index != SnapshotIndex ||
            snapshot.Term != SnapshotTerm || snapshot.Term > CurrentTerm || snapshot.Voters != voters)
            throw std::invalid_argument("Legacy snapshot does not match the trusted voter baseline");
    } else if (SnapshotIndex || SnapshotTerm) {
        throw std::invalid_argument("Legacy compacted state is missing its snapshot");
    }

    auto previousTerm = SnapshotTerm;
    for (uint64_t index = SnapshotIndex + 1; index <= LastLogIndex; ++index) {
        if (RetainedLogFlags(index) & TLogEntry::EConfiguration)
            throw std::invalid_argument("Configuration log migration requires an explicit history baseline");
        const auto term = RetainedLogTerm(index);
        if (!term || term < previousTerm || term > CurrentTerm)
            throw std::invalid_argument("Invalid legacy log term sequence");
        previousTerm = term;
    }
    if (LastLogTerm != previousTerm)
        throw std::invalid_argument("Legacy log last term mismatch");

    // Persist the operator-supplied baseline before Raft can serve votes or
    // install configuration snapshots against it.
    Impl->Voters = voters;
    Commit();
}
