#pragma once

#include "state.h"

#include <fstream>
#include <string>
#include <vector>

// Files: <name>.state.<id> (metadata, the commit point), and <name>.entries.<id>,
// <name>.index.<id> for the log; after a snapshot at index X the log is in
// <name>.entries.<id>.X, <name>.index.<id>.X and the image in <name>.snapshot.<id>.X.
struct TDiskState : IState {
    TDiskState(const std::string& name, uint32_t id);
    
    void RemoveLast() override;
    void Append(TMessageHolder<TLogEntry> entry) override;
    TMessageHolder<TLogEntry> Get(int64_t index) const override;
    void Commit() override;
    TMessageHolder<TInstallSnapshotRequest> GetSnapshot() const override;
    void InstallSnapshot(TMessageHolder<TInstallSnapshotRequest> snapshot) override;

private:
    [[noreturn]] void Fail(const char* message) const;
    std::fstream Open(const std::string& name, bool create);
    std::string Path(const std::string& kind, uint64_t snapshotIndex) const;
    std::vector<std::pair<std::string, uint64_t>> Files() const;
    TMessageHolder<TMessage> Read() const;
    void Write(TMessageHolder<TMessage>);

    std::string Name;
    uint32_t Id;
    mutable std::fstream Entries;
    mutable std::fstream Index;
    std::fstream State;
};
