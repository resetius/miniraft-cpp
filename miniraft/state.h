#pragma once

#include <stdexcept>

#include "messages.h"

struct TStateFields {
    uint64_t CurrentTerm = 1;
    uint32_t VotedFor = 0;
    uint64_t LastLogIndex = 0;
    uint64_t LastLogTerm = 0;
    // Last entry covered by the snapshot; the log holds (SnapshotIndex, LastLogIndex].
    uint64_t SnapshotIndex = 0;
    uint64_t SnapshotTerm = 0;

    bool operator==(const TStateFields& other) const = default;
};

struct IState: TStateFields {
    virtual void RemoveLast() = 0;
    virtual void Append(TMessageHolder<TLogEntry>) = 0;
    // index starts from 1, entries up to SnapshotIndex are gone
    virtual TMessageHolder<TLogEntry> Get(int64_t index) const = 0;
    virtual void Commit() = 0;
    // The image covering (0, SnapshotIndex], empty without a snapshot.
    virtual TMessageHolder<TInstallSnapshotRequest> GetSnapshot() const = 0;
    // Publishes an image with LastIncludedIndex > SnapshotIndex atomically: the entries
    // it covers are dropped, the later ones stay only if the entry at it has its term.
    // Any exception is a storage failure: it reaches the embedder and the leader resends the last chunk.
    virtual void InstallSnapshot(TMessageHolder<TInstallSnapshotRequest> snapshot) = 0;
    virtual ~IState() = default;

    // Zero for an index that is not in the log, including one before the snapshot.
    uint64_t LogTerm(int64_t index = -1) const {
        if (index < 0) {
            index = LastLogIndex;
        }
        if (uint64_t(index) == SnapshotIndex) {
            return SnapshotTerm;
        }
        if (uint64_t(index) < SnapshotIndex || uint64_t(index) > LastLogIndex) {
            return 0;
        } else {
            return Get(index)->Term;
        }
    }
};

struct TState: IState {
    std::vector<TMessageHolder<TLogEntry>> Log;
    TMessageHolder<TInstallSnapshotRequest> Snapshot;

    TState() = default;
    TState(uint64_t currentTerm, uint32_t votedFor, const std::vector<TMessageHolder<TLogEntry>>& log)
        : Log(log)
    {
        CurrentTerm = currentTerm;
        VotedFor = votedFor;
        if (!log.empty()) {
            LastLogIndex = log.size();
            LastLogTerm = log.back()->Term;
        }
    }

    void RemoveLast() override {
        Log.pop_back();
        LastLogIndex = SnapshotIndex + Log.size();
        LastLogTerm = Log.empty() ? SnapshotTerm : Log.back()->Term;
    }

    void Append(TMessageHolder<TLogEntry> entry) override {
        Log.emplace_back(std::move(entry));
        LastLogIndex = SnapshotIndex + Log.size();
        LastLogTerm = Log.back()->Term;
    }

    TMessageHolder<TLogEntry> Get(int64_t index) const override {
        return Log[index - SnapshotIndex - 1];
    }

    void Commit() override { }

    TMessageHolder<TInstallSnapshotRequest> GetSnapshot() const override {
        return Snapshot;
    }

    void InstallSnapshot(TMessageHolder<TInstallSnapshotRequest> snapshot) override {
        const auto index = snapshot->LastIncludedIndex;
        const auto term = snapshot->LastIncludedTerm;
        if (index <= SnapshotIndex) {
            throw std::invalid_argument("Snapshot does not advance the log");
        }
        if (index <= LastLogIndex && LogTerm(index) == term) {
            Log.erase(Log.begin(), Log.begin() + (index - SnapshotIndex));
        } else {
            Log.clear();
        }
        SnapshotIndex = index;
        SnapshotTerm = term;
        Snapshot = std::move(snapshot);
        LastLogIndex = SnapshotIndex + Log.size();
        LastLogTerm = Log.empty() ? SnapshotTerm : Log.back()->Term;
    }
};

