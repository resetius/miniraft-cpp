#pragma once
#include "state.h"
#include <functional>
#include <memory>
#include <string>

// Linux: atomic hard-state + snapshot + suffix replacement, with file/directory
// fsync. O(snapshot + retained log size) per update: a durability baseline.
class TDurableState : public IState {
public:
    enum class EStage { TempCreated, Written, FileSynced, Renamed, DirectorySynced };
    // Diagnostics/fault injection. Throwing from this hook during a live
    // operation is fatal, just like an I/O failure.
    using THook = std::function<void(EStage)>;
    TDurableState(const std::string& name, uint32_t id, THook hook = {});
    ~TDurableState() override;
    TDurableState(const TDurableState&) = delete;
    TDurableState& operator=(const TDurableState&) = delete;
    void RemoveLast() override;
    void Append(TMessageHolder<TLogEntry> entry) override;
    TMessageHolder<TLogEntry> Get(int64_t index) const override;
    uint64_t RetainedLogTerm(int64_t index) const override;
    uint64_t RetainedLogFlags(int64_t index) const override;
    void Commit() override;
    bool SupportsSnapshots() const override { return true; }
    std::shared_ptr<const TSnapshot> GetSnapshot() const override;
    void InstallSnapshot(const TSnapshot& snapshot) override;
    bool SupportsVoterBinding() const override { return true; }
    bool HasUnboundRecoveredState() const override;
    std::vector<uint32_t> GetBoundVoters() const override;
    void BindVoters(const std::vector<uint32_t>& voters) override;
    void MarkRaftStarted() override;
    // Explicit legacy migration. Call before constructing TRaft. The caller
    // asserts that voters is the complete original fixed set, verified outside
    // this store.
    void BindVotersFromTrustedBaseline(const std::vector<uint32_t>& voters);
private:
    struct TImpl;
    std::unique_ptr<TImpl> Impl;
    void Publish();
};
