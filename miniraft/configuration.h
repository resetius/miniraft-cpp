#pragma once

#include "messages.h"
#include "snapshot.h"
#include <cstdint>
#include <vector>

// Experimental record encoding only. Current Raft rejects these entries.
enum class EConfigurationPhase : uint32_t { Joint = 1, Stable = 2 };
struct TConfigurationRecord {
    EConfigurationPhase Phase = EConfigurationPhase::Joint;
    uint64_t Term = 0, PreviousIndex = 0, PreviousTerm = 0;
    std::vector<uint32_t> OldVoters, NewVoters;
    bool operator==(const TConfigurationRecord&) const = default;
};

TMessageHolder<TLogEntry> EncodeConfigurationEntry(const TConfigurationRecord& record);
TConfigurationRecord DecodeConfigurationEntry(TMessageHolder<TLogEntry> entry);

// Offline transition validation only; does not establish commitment, activate
// roles, or make a fixed-membership Raft node accept configuration entries.
class TConfigurationHistory {
public:
    TConfigurationHistory(const TConfigurationHistory&) = default;
    TConfigurationHistory& operator=(const TConfigurationHistory&) = default;
    // The caller must supply a trusted identity and scanned-prefix boundary.
    // A zero configuration identity asserts that no configuration entry exists
    // in that prefix; a v1 snapshot alone does not prove this assertion.
    static TConfigurationHistory Stable(std::vector<uint32_t> voters,
        uint64_t configurationIndex, uint64_t configurationTerm,
        uint64_t throughIndex, uint64_t throughTerm);
    static TConfigurationHistory FromJointSnapshot(const TSnapshot& snapshot);
    static TConfigurationHistory FromStableSnapshot(const TSnapshot& snapshot);
    // Visit every retained entry in index order; neither method changes *this.
    TConfigurationHistory Observe(uint64_t index, TMessageHolder<TLogEntry> entry) const;
    // Metadata must come from a trusted, already validated storage backend.
    TConfigurationHistory ObserveMetadata(uint64_t index, uint64_t term, uint64_t flags) const;
    TConfigurationHistory Advance(uint64_t index, TMessageHolder<TLogEntry> entry) const;
    // Offline projection only. The caller must establish that ThroughIndex is
    // committed and that data is the application image at exactly that index.
    TSnapshot MakeSnapshot(std::string data) const;

    bool IsJoint() const { return !New.empty(); }
    const std::vector<uint32_t>& OldVoters() const { return Old; }
    const std::vector<uint32_t>& NewVoters() const { return New; }
    uint64_t ConfigurationIndex() const { return ConfigIndex; }
    uint64_t ConfigurationTerm() const { return ConfigTerm; }
    uint64_t ThroughIndex() const { return Through; }
    uint64_t ThroughTerm() const { return ThroughLogTerm; }
private:
    TConfigurationHistory() = default;
    std::vector<uint32_t> Old, New;
    uint64_t ConfigIndex = 0, ConfigTerm = 0, Through = 0, ThroughLogTerm = 0;
};

struct IState;
struct TConfigurationScan {
    TConfigurationHistory Committed;
    TConfigurationHistory Latest;
    bool HasUncommittedConfiguration() const {
        return Committed.ConfigurationIndex() != Latest.ConfigurationIndex() ||
            Committed.ConfigurationTerm() != Latest.ConfigurationTerm() ||
            Committed.IsJoint() != Latest.IsJoint() ||
            Committed.OldVoters() != Latest.OldVoters() ||
            Committed.NewVoters() != Latest.NewVoters();
    }
};

// Offline, read-only validation. The caller supplies and must substantiate both
// the baseline and the commit boundary; neither is proven by this scan.
TConfigurationScan ScanConfigurationHistory(const IState& state,
    TConfigurationHistory baseline, uint64_t commitIndex);
