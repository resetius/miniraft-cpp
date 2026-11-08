#pragma once

#include <unordered_map>
#include <string>

#include <miniraft/raft.h>

class TKv: public IRsm {
public:
    void Validate(TMessageHolder<TCommandRequest> message) const override;
    TMessageHolder<TMessage> Read(TMessageHolder<TCommandRequest> message, uint64_t index) override;
    TMessageHolder<TMessage> Write(TMessageHolder<TLogEntry> message, uint64_t index) override;
    TMessageHolder<TLogEntry> Prepare(TMessageHolder<TCommandRequest> message) override;

private:
    std::unordered_map<std::string, std::string> H;
};
