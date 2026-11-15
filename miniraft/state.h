#pragma once

#include "messages.h"
#include "snapshot.h"
#include <algorithm>

struct TStateFields {
    uint64_t CurrentTerm = 1;
    uint32_t VotedFor = 0;
    uint64_t LastLogIndex = 0;
    uint64_t LastLogTerm = 0;
    uint64_t SnapshotIndex = 0;
    uint64_t SnapshotTerm = 0;

    bool operator==(const TStateFields& other) const = default;
};

struct IState: TStateFields {
    virtual void RemoveLast() = 0;
    virtual void Append(TMessageHolder<TLogEntry>) = 0;
    // Absolute indices; only the suffix after SnapshotIndex has log entries.
    // The boundary term remains available through LogTerm(SnapshotIndex).
    virtual TMessageHolder<TLogEntry> Get(int64_t index) const = 0;
    // Backends may read a retained entry's term without copying its payload.
    // The default keeps existing external backends source compatible.
    virtual uint64_t RetainedLogTerm(int64_t index) const {
        auto entry = Get(index);
        return entry ? entry->Term : 0;
    }
    // Used during startup to reject unsupported retained entry kinds.
    virtual uint64_t RetainedLogFlags(int64_t index) const {
        auto entry = Get(index);
        if (!entry) throw std::runtime_error("Missing retained log entry");
        return entry->Flags;
    }
    virtual void Commit() = 0;
    virtual bool SupportsSnapshots() const { return false; }
    virtual std::shared_ptr<const TSnapshot> GetSnapshot() const { return {}; }
    virtual void InstallSnapshot(const TSnapshot&) { throw std::logic_error("Storage snapshots are unsupported"); }
    virtual bool SupportsVoterBinding() const { return false; }
    // True when this backend recovered a persistent image without a stored
    // voter identity; Raft must obtain proof before serving this state.
    virtual bool HasUnboundRecoveredState() const { return false; }
    virtual std::vector<uint32_t> GetBoundVoters() const { return {}; }
    virtual void BindVoters(const std::vector<uint32_t>&) { throw std::logic_error("Storage voter binding is unsupported"); }
    // Storage backends can close startup-only migration paths once a Raft
    // instance has finished validating and attached to this state.
    virtual void MarkRaftStarted() {}
    virtual ~IState() = default;

    uint64_t LogTerm(int64_t index = -1) const {
        if (index < 0) {
            index = LastLogIndex;
        }
        if (index > 0 && uint64_t(index) == SnapshotIndex) return SnapshotTerm;
        if (index < 1 || uint64_t(index) < SnapshotIndex || index > LastLogIndex) {
            return 0;
        } else {
            return RetainedLogTerm(index);
        }
    }
};

inline void ValidateFixedVoters(const std::vector<uint32_t>& voters) {
    if (voters.empty() || voters.size() > TSnapshot::MaxVoters) throw std::invalid_argument("Invalid voter count");
    uint32_t previous = 0;
    for (auto id : voters) {
        if (id <= previous) throw std::invalid_argument("Voters must be positive, sorted and unique");
        previous = id;
    }
}
inline void ValidateVoterBinding(const IState& state, const std::vector<uint32_t>& old, const std::vector<uint32_t>& voters) {
    ValidateFixedVoters(voters);
    if (state.VotedFor && !std::binary_search(voters.begin(), voters.end(), state.VotedFor))
        throw std::invalid_argument("Stored vote does not name a configured voter");
    if (!old.empty()) {
        if (old != voters) throw std::invalid_argument("Persisted voter configuration cannot be changed by restart");
    } else if (auto snapshot = state.GetSnapshot()) {
        if (snapshot->HasConfigurationMetadata())
            throw std::invalid_argument("Configuration snapshot recovery requires an existing voter binding");
        if (snapshot->Voters != voters) throw std::invalid_argument("Snapshot voter configuration mismatch");
    } else if (state.CurrentTerm != 1 || state.VotedFor || state.LastLogIndex) {
        throw std::invalid_argument("Unbound nonfresh store needs a verified snapshot before enabling learners");
    }
}

struct TState: IState {
    static TMessageHolder<TLogEntry> CloneEntry(const TMessageHolder<TLogEntry>& entry) {
        if (!entry.HasValidExtent() || entry->Type != uint32_t(EMessageType::LOG_ENTRY) || entry->Len < sizeof(TLogEntry))
            throw std::invalid_argument("Invalid in-memory log entry");
        auto copy = NewHoldedMessage<TLogEntry>(entry->Len);
        memcpy(copy.Mes, entry.Mes, entry->Len);
        return copy;
    }
    std::vector<TMessageHolder<TLogEntry>> Log;
    std::shared_ptr<const TSnapshot> Snapshot;
    std::vector<uint32_t> BoundVoters;

    TState() = default;
    TState(uint64_t currentTerm, uint32_t votedFor, const std::vector<TMessageHolder<TLogEntry>>& log)
    {
        for (const auto& entry : log) Log.push_back(CloneEntry(entry));
        CurrentTerm = currentTerm;
        VotedFor = votedFor;
        if (!log.empty()) {
            LastLogIndex = log.size();
            LastLogTerm = log.back()->Term;
        }
    }
    TState(const TState& other) : IState(other), Snapshot(other.Snapshot), BoundVoters(other.BoundVoters) {
        for (const auto& entry : other.Log) Log.push_back(CloneEntry(entry));
    }
    TState& operator=(const TState& other) {
        if (this != &other) {
            TState copy(other);
            static_cast<TStateFields&>(*this) = copy;
            Log.swap(copy.Log); Snapshot.swap(copy.Snapshot); BoundVoters.swap(copy.BoundVoters);
        }
        return *this;
    }

    void RemoveLast() override {
        if (Log.empty()) return;
        Log.pop_back();
        LastLogIndex = SnapshotIndex + Log.size();
        LastLogTerm = Log.empty() ? SnapshotTerm : Log.back()->Term;
    }

    void Append(TMessageHolder<TLogEntry> entry) override {
        if (LastLogIndex >= uint64_t(INT64_MAX)) throw std::overflow_error("Raft log index exhausted");
        Log.emplace_back(CloneEntry(entry));
        LastLogIndex = SnapshotIndex + Log.size();
        LastLogTerm = Log.back()->Term;
    }

    TMessageHolder<TLogEntry> Get(int64_t index) const override {
        if (index < 1 || uint64_t(index) <= SnapshotIndex || uint64_t(index) > LastLogIndex) return {};
        return CloneEntry(Log[index - SnapshotIndex - 1]);
    }
    uint64_t RetainedLogTerm(int64_t index) const override {
        if (index < 1 || uint64_t(index) <= SnapshotIndex || uint64_t(index) > LastLogIndex) return 0;
        return Log[index - SnapshotIndex - 1]->Term;
    }
    uint64_t RetainedLogFlags(int64_t index) const override {
        if (index < 1 || uint64_t(index) <= SnapshotIndex || uint64_t(index) > LastLogIndex)
            throw std::out_of_range("Missing retained log entry");
        return Log[index - SnapshotIndex - 1]->Flags;
    }

    bool SupportsSnapshots() const override { return true; }
    bool SupportsVoterBinding() const override { return true; }
    std::vector<uint32_t> GetBoundVoters() const override { return BoundVoters; }
    void BindVoters(const std::vector<uint32_t>& voters) override {
        ValidateVoterBinding(*this, BoundVoters, voters);
        BoundVoters = voters;
    }
    std::shared_ptr<const TSnapshot> GetSnapshot() const override { return Snapshot; }
    void InstallSnapshot(const TSnapshot& snapshot) override {
        ValidateSnapshot(snapshot);
        TSnapshot image = snapshot;
        PreserveStableSnapshotIdentity(Snapshot.get(), image);
        ValidateSnapshot(image); // Validate before changing any state.
        if (image.HasConfigurationMetadata() && BoundVoters.empty())
            throw std::invalid_argument("Configuration snapshots require a bound voter set");
        if (!BoundVoters.empty() && image.Voters != BoundVoters)
            throw std::invalid_argument("Snapshot would change bound voters");
        if (image.Index <= SnapshotIndex || image.Term < SnapshotTerm || image.Term > CurrentTerm)
            throw std::invalid_argument("Obsolete or future snapshot");
        auto saved = std::make_shared<const TSnapshot>(std::move(image));
        std::vector<TMessageHolder<TLogEntry>> suffix;
        if (saved->Index <= LastLogIndex && LogTerm(saved->Index) == saved->Term)
            suffix.assign(Log.begin() + (saved->Index - SnapshotIndex), Log.end());
        Log.swap(suffix);
        Snapshot = std::move(saved);
        SnapshotIndex = Snapshot->Index;
        SnapshotTerm = Snapshot->Term;
        LastLogIndex = SnapshotIndex + Log.size();
        LastLogTerm = Log.empty() ? SnapshotTerm : Log.back()->Term;
    }

    void Commit() override { }
};

