#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class TRaft;
struct IRsm;

// Versioned application snapshot, shared by local compaction and state transfer.
struct TSnapshot {
    static constexpr size_t MaxBytes = 64 * 1024 * 1024;
    static constexpr uint32_t MaxVoters = 4096;
    uint64_t Index = 0, Term = 0;
    std::vector<uint32_t> Voters;
    std::string Data;
    // Optional joint configuration effective at this snapshot boundary.
    // Runtime installation is disabled until membership recovery is implemented.
    std::vector<uint32_t> JointVoters;
    uint64_t ConfigurationIndex = 0, ConfigurationTerm = 0;
    // Experimental stable-configuration identity. Offline storage may retain
    // an active voter set that differs from its original persisted binding;
    // live Raft still rejects that configuration until transitions are enabled.
    bool StableConfiguration = false;
    bool IsJoint() const { return !JointVoters.empty() || (!StableConfiguration && (ConfigurationIndex || ConfigurationTerm)); }
    bool HasConfigurationMetadata() const { return StableConfiguration || IsJoint(); }
    bool operator==(const TSnapshot&) const = default;
};
std::string EncodeSnapshot(const TSnapshot& snapshot);
TSnapshot DecodeSnapshot(std::string_view bytes);
// Validate metadata and size without copying the application image.
void ValidateSnapshot(const TSnapshot& snapshot);
// Preserve a trusted stable configuration identity when a legacy image is
// refreshed, and reject a stable image that changes that identity.
void PreserveStableSnapshotIdentity(const TSnapshot* current, TSnapshot& incoming);
TSnapshot CaptureSnapshot(const TRaft& raft, const IRsm& rsm);
// Startup only, with an application behind the snapshot and
// a matching local boundary/log. This can recover an app left behind by a crash
// after durable Raft snapshot publication.
void RestoreSnapshot(const TSnapshot& snapshot, const TRaft& raft, IRsm& rsm);

class TSnapshotStore {
public:
    enum class EStage { TempCreated, Written, FileSynced, Renamed, DirectorySynced };
    using THook = std::function<void(EStage)>;
    // Linux atomic publication, exclusive writer lock, bounded reads and CRC32.
    TSnapshotStore(const std::string& name, uint32_t id, THook hook = {});
    ~TSnapshotStore();
    TSnapshotStore(const TSnapshotStore&) = delete;
    TSnapshotStore& operator=(const TSnapshotStore&) = delete;
    std::optional<TSnapshot> Load();
    // Standalone checkpoint export, not the compaction commit point. Failure is
    // catchable when the authoritative snapshot/log remains in the Raft state.
    void Save(const TSnapshot& snapshot);
private:
    struct TImpl;
    std::unique_ptr<TImpl> Impl;
};
