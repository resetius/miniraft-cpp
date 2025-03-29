#pragma once

#include <unordered_map>
#include <string>

#include <miniraft/raft.h>

class TKv: public IRsm {
public:
    TMessageHolder<TMessage> Read(TMessageHolder<TCommandRequest> message, uint64_t index) override;
    TMessageHolder<TMessage> Write(TMessageHolder<TLogEntry> message, uint64_t index) override;
    TMessageHolder<TLogEntry> Prepare(TMessageHolder<TCommandRequest> message) override;
    void Apply(TMessageHolder<TInstallSnapshotRequest> snapshot) override {
        throw std::runtime_error("Not implemented");
    }

private:
    std::unordered_map<std::string, std::string> H;
};
