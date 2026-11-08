#pragma once

#include "state.h"

#include <fstream>
#include <string>

struct TDiskState : IState {
    TDiskState(const std::string& name, uint32_t id);
    
    void RemoveLast() override;
    void Append(TMessageHolder<TLogEntry> entry) override;
    TMessageHolder<TLogEntry> Get(int64_t index) const override;
    void Commit() override;

private:
    bool Recovering = true;
    [[noreturn]] void Fail(const char* message) const;
    std::fstream Open(const std::string& name, bool create);
    TMessageHolder<TMessage> Read() const;
    void Write(TMessageHolder<TMessage>);

    mutable std::fstream Entries;
    mutable std::fstream Index;
    std::fstream State;
};
