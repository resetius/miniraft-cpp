#include "configuration.h"
#include "snapshot_codec.h"
#include "state.h"
#include <climits>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {
using namespace NSnapshotCodec;
constexpr std::string_view Magic = "MRFTC001";
constexpr size_t MaxPayload = 48 + 8 * TSnapshot::MaxVoters;
void Validate(const TConfigurationRecord& record) {
    if ((record.Phase != EConfigurationPhase::Joint && record.Phase != EConfigurationPhase::Stable) ||
        !record.Term || record.PreviousIndex > uint64_t(INT64_MAX) ||
        ((record.PreviousIndex == 0) != (record.PreviousTerm == 0)) ||
        record.PreviousTerm > record.Term ||
        (record.Phase == EConfigurationPhase::Stable && !record.PreviousIndex))
        throw std::invalid_argument("Invalid configuration entry metadata");
    ValidateFixedVoters(record.OldVoters);
    ValidateFixedVoters(record.NewVoters);
}
}

TMessageHolder<TLogEntry> EncodeConfigurationEntry(const TConfigurationRecord& record) {
    Validate(record);
    std::string bytes(Magic);
    Put(bytes, uint32_t(record.Phase), 4);
    Put(bytes, record.Term, 8);
    Put(bytes, record.PreviousIndex, 8);
    Put(bytes, record.PreviousTerm, 8);
    Put(bytes, record.OldVoters.size(), 4);
    Put(bytes, record.NewVoters.size(), 4);
    for (auto id : record.OldVoters) Put(bytes, id, 4);
    for (auto id : record.NewVoters) Put(bytes, id, 4);
    Put(bytes, Checksum(bytes), 4);
    auto entry = NewHoldedMessage<TLogEntry>(sizeof(TLogEntry) + bytes.size());
    entry->Term = record.Term; entry->Flags = TLogEntry::EConfiguration;
    memcpy(entry->Data, bytes.data(), bytes.size());
    return entry;
}

TConfigurationRecord DecodeConfigurationEntry(TMessageHolder<TLogEntry> entry) {
    if (!entry.HasValidExtent() || entry->Type != uint32_t(EMessageType::LOG_ENTRY) ||
        entry->Len < sizeof(TLogEntry) || entry->Flags != TLogEntry::EConfiguration || entry.PayloadSize)
        throw std::invalid_argument("Invalid configuration entry frame");
    const auto length = entry->Len - sizeof(TLogEntry);
    if (length < 56 || length > MaxPayload) throw std::invalid_argument("Invalid configuration entry size");
    const std::string_view bytes(entry->Data, length);
    TReader trailer{bytes.substr(length - 4)};
    if (Checksum(bytes.substr(0, length - 4)) != trailer.Take(4))
        throw std::invalid_argument("Configuration entry checksum mismatch");
    TReader reader{bytes.substr(0, length - 4)};
    if (reader.Read(Magic.size()) != Magic) throw std::invalid_argument("Unknown configuration entry format");
    TConfigurationRecord record;
    record.Phase = EConfigurationPhase(reader.Take(4));
    record.Term = reader.Take(8); record.PreviousIndex = reader.Take(8); record.PreviousTerm = reader.Take(8);
    const auto oldCount = reader.Take(4), newCount = reader.Take(4);
    if (oldCount > TSnapshot::MaxVoters || newCount > TSnapshot::MaxVoters ||
        oldCount + newCount > reader.Input.size() / 4)
        throw std::invalid_argument("Invalid configuration voter counts");
    for (uint64_t i = 0; i < oldCount; ++i) record.OldVoters.push_back(reader.Take(4));
    for (uint64_t i = 0; i < newCount; ++i) record.NewVoters.push_back(reader.Take(4));
    if (!reader.Input.empty() || entry->Term != record.Term)
        throw std::invalid_argument("Configuration entry term or length mismatch");
    Validate(record);
    return record;
}

TConfigurationHistory TConfigurationHistory::Stable(std::vector<uint32_t> voters,
    uint64_t configurationIndex, uint64_t configurationTerm, uint64_t throughIndex, uint64_t throughTerm) {
    ValidateFixedVoters(voters);
    if (throughIndex > uint64_t(INT64_MAX) || configurationIndex > throughIndex ||
        ((configurationIndex == 0) != (configurationTerm == 0)) ||
        ((throughIndex == 0) != (throughTerm == 0)) || configurationTerm > throughTerm ||
        (configurationIndex && configurationIndex == throughIndex && configurationTerm != throughTerm))
        throw std::invalid_argument("Invalid configuration history baseline");
    TConfigurationHistory history;
    history.Old = std::move(voters); history.ConfigIndex = configurationIndex;
    history.ConfigTerm = configurationTerm; history.Through = throughIndex; history.ThroughLogTerm = throughTerm;
    return history;
}

TConfigurationHistory TConfigurationHistory::FromJointSnapshot(const TSnapshot& snapshot) {
    ValidateSnapshot(snapshot);
    if (!snapshot.IsJoint() || snapshot.StableConfiguration) throw std::invalid_argument("Snapshot does not identify a joint configuration");
    TConfigurationHistory history;
    history.Old = snapshot.Voters; history.New = snapshot.JointVoters;
    history.ConfigIndex = snapshot.ConfigurationIndex; history.ConfigTerm = snapshot.ConfigurationTerm;
    history.Through = snapshot.Index; history.ThroughLogTerm = snapshot.Term;
    return history;
}

TConfigurationHistory TConfigurationHistory::FromStableSnapshot(const TSnapshot& snapshot) {
    ValidateSnapshot(snapshot);
    if (!snapshot.StableConfiguration) throw std::invalid_argument("Snapshot does not identify a stable configuration");
    return Stable(snapshot.Voters, snapshot.ConfigurationIndex, snapshot.ConfigurationTerm, snapshot.Index, snapshot.Term);
}

TSnapshot TConfigurationHistory::MakeSnapshot(std::string data) const {
    TSnapshot snapshot{Through, ThroughLogTerm, Old, std::move(data)};
    if (IsJoint()) {
        snapshot.JointVoters = New;
        snapshot.ConfigurationIndex = ConfigIndex; snapshot.ConfigurationTerm = ConfigTerm;
    } else if (ConfigIndex) {
        snapshot.StableConfiguration = true;
        snapshot.ConfigurationIndex = ConfigIndex; snapshot.ConfigurationTerm = ConfigTerm;
    }
    ValidateSnapshot(snapshot);
    return snapshot;
}

TConfigurationHistory TConfigurationHistory::Advance(uint64_t index, TMessageHolder<TLogEntry> entry) const {
    const auto record = DecodeConfigurationEntry(std::move(entry));
    if (Through >= uint64_t(INT64_MAX) || index != Through + 1 || record.Term < ThroughLogTerm ||
        record.PreviousIndex != ConfigIndex || record.PreviousTerm != ConfigTerm)
        throw std::invalid_argument("Configuration entry does not extend the trusted history");
    TConfigurationHistory next = *this;
    if (!IsJoint()) {
        if (record.Phase != EConfigurationPhase::Joint || record.OldVoters != Old)
            throw std::invalid_argument("Expected a joint configuration from the stable voter set");
        next.New = record.NewVoters;
    } else {
        if (record.Phase != EConfigurationPhase::Stable || record.OldVoters != Old || record.NewVoters != New)
            throw std::invalid_argument("Expected the stable successor of the joint configuration");
        next.Old = New; next.New.clear();
    }
    next.ConfigIndex = next.Through = index;
    next.ConfigTerm = next.ThroughLogTerm = record.Term;
    return next;
}

TConfigurationHistory TConfigurationHistory::Observe(uint64_t index, TMessageHolder<TLogEntry> entry) const {
    if (!entry.HasValidExtent() || entry->Type != uint32_t(EMessageType::LOG_ENTRY) ||
        entry->Len < sizeof(TLogEntry) || entry.PayloadSize)
        throw std::invalid_argument("Invalid ordinary log entry");
    return ObserveMetadata(index, entry->Term, entry->Flags);
}

TConfigurationHistory TConfigurationHistory::ObserveMetadata(uint64_t index, uint64_t term, uint64_t flags) const {
    if ((flags & TLogEntry::EConfiguration) || !term ||
        Through >= uint64_t(INT64_MAX) || index != Through + 1 || term < ThroughLogTerm)
        throw std::invalid_argument("Ordinary entry does not extend the trusted history");
    TConfigurationHistory next = *this;
    next.Through = index; next.ThroughLogTerm = term;
    return next;
}

TConfigurationScan ScanConfigurationHistory(const IState& state,
    TConfigurationHistory baseline, uint64_t commitIndex) {
    if (!state.CurrentTerm || state.LastLogIndex > uint64_t(INT64_MAX) ||
        state.SnapshotIndex > state.LastLogIndex || state.SnapshotTerm > state.CurrentTerm ||
        state.LastLogTerm > state.CurrentTerm ||
        baseline.ThroughIndex() != state.SnapshotIndex || baseline.ThroughTerm() != state.SnapshotTerm ||
        commitIndex < state.SnapshotIndex || commitIndex > state.LastLogIndex)
        throw std::invalid_argument("Invalid configuration scan boundaries");

    const auto snapshot = state.GetSnapshot();
    if (state.SnapshotIndex) {
        if (!snapshot) throw std::invalid_argument("Missing configuration scan snapshot");
        ValidateSnapshot(*snapshot);
        if (snapshot->Index != state.SnapshotIndex || snapshot->Term != state.SnapshotTerm)
            throw std::invalid_argument("Configuration scan snapshot boundary mismatch");
        if (snapshot->IsJoint()) {
            const auto expected = TConfigurationHistory::FromJointSnapshot(*snapshot);
            if (!baseline.IsJoint() || baseline.OldVoters() != expected.OldVoters() ||
                baseline.NewVoters() != expected.NewVoters() ||
                baseline.ConfigurationIndex() != expected.ConfigurationIndex() ||
                baseline.ConfigurationTerm() != expected.ConfigurationTerm())
                throw std::invalid_argument("Joint snapshot baseline mismatch");
        } else if (baseline.IsJoint() || baseline.OldVoters() != snapshot->Voters ||
            (snapshot->StableConfiguration && (baseline.ConfigurationIndex() != snapshot->ConfigurationIndex ||
                baseline.ConfigurationTerm() != snapshot->ConfigurationTerm))) {
            throw std::invalid_argument("Fixed snapshot baseline mismatch");
        }
    } else if (snapshot || state.SnapshotTerm || baseline.IsJoint()) {
        throw std::invalid_argument("Invalid empty snapshot boundary");
    }
    const auto bound = state.GetBoundVoters();
    if (!bound.empty() && bound != baseline.OldVoters())
        throw std::invalid_argument("Configuration baseline differs from bound voters");

    auto latest = baseline;
    auto committed = baseline;
    for (uint64_t index = state.SnapshotIndex + 1; index <= state.LastLogIndex; ++index) {
        const auto term = state.RetainedLogTerm(index);
        const auto flags = state.RetainedLogFlags(index);
        if (!term || term > state.CurrentTerm) throw std::invalid_argument("Invalid retained log term");
        if (flags & TLogEntry::EConfiguration) {
            auto entry = state.Get(index);
            if (!entry.HasValidExtent() || entry->Len < sizeof(TLogEntry) ||
                entry->Term != term || entry->Flags != flags)
                throw std::invalid_argument("Inconsistent configuration entry metadata");
            latest = latest.Advance(index, entry);
        } else {
            latest = latest.ObserveMetadata(index, term, flags);
        }
        if (index == commitIndex) committed = latest;
    }
    if (latest.ThroughTerm() != state.LastLogTerm)
        throw std::invalid_argument("Configuration scan last term mismatch");
    return {committed, latest};
}

TConfigurationScan ScanCommittedConfigurationHistory(const IState& state,
    TConfigurationHistory baseline) {
    if (!state.SupportsCommitIndexPersistence() || !state.SupportsVoterBinding() ||
        state.GetBoundVoters().empty())
        throw std::invalid_argument("Committed configuration scan requires durable commit and voter metadata");
    return ScanConfigurationHistory(state, std::move(baseline), state.CommitIndex);
}
