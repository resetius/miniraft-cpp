#pragma once

#include "messages.h"

struct IState {
    uint64_t CurrentTerm = 1;
    uint32_t VotedFor = 0;
    uint64_t LastLogIndex = 0;
    uint64_t LastLogTerm = 0;

    virtual void RemoveLast() = 0;
    virtual void Append(TMessageHolder<TLogEntry>) = 0;
    virtual TMessageHolder<TLogEntry> Get(int64_t index) const = 0;
    virtual void Commit() = 0;
    virtual ~IState() = default;

    uint64_t LogTerm(int64_t index = -1) const {
        if (index < 0) {
            index = LastLogIndex;
        }
        if (index < 1 || index > LastLogIndex) {
            return 0;
        } else {
            return Get(index-1)->Term;
        }
    }
};

struct TState: IState {
    std::vector<TMessageHolder<TLogEntry>> Log;

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
        LastLogIndex = Log.size();
        LastLogTerm = Log.empty() ? 0 : Log.back()->Term;
    }

    void Append(TMessageHolder<TLogEntry> entry) override {
        Log.emplace_back(std::move(entry));
        LastLogIndex = Log.size();
        LastLogTerm = Log.back()->Term;
    }

    TMessageHolder<TLogEntry> Get(int64_t index) const override {
        return Log[index];
    }

    void Commit() override { }
};

