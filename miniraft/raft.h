#pragma once

#include <memory>
#include <queue>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "messages.h"
#include "timesource.h"
#include "state.h"
#include "quorum.h"

struct INode {
    virtual ~INode() = default;
    virtual void Send(TMessageHolder<TMessage> message) = 0;
    virtual void Drain() = 0;
};

// CommandRequest -> Write? -> LogEntry -> Append -> Committed As Index -> Applied As Index (Same) -> Index -> CommandResponse
// CommandRequest -> Read? -> CurrentIndex (fixate) >= CommittedIndex -> CommandResponse
struct IRsm {
    virtual ~IRsm() = default;
    // Check application wire shape before queueing/forwarding; must not mutate state.
    virtual void Validate(TMessageHolder<TCommandRequest>) const {}
    virtual TMessageHolder<TMessage> Read(TMessageHolder<TCommandRequest> message, uint64_t index) = 0;
    virtual TMessageHolder<TMessage> Write(TMessageHolder<TLogEntry> message, uint64_t index) = 0;
    virtual TMessageHolder<TLogEntry> Prepare(TMessageHolder<TCommandRequest> message) = 0;
    virtual std::string SaveSnapshot() const { throw std::logic_error("Application snapshots are unsupported"); }
    virtual void ValidateSnapshot(std::string_view, uint64_t) const { throw std::logic_error("Application snapshots are unsupported"); }
    // Implementations must validate completely before atomically replacing state
    // and LastAppliedIndex. Invoked at startup or during serialized installation;
    // a failure after durable installation is fatal and requires restart.
    virtual void LoadSnapshot(std::string_view, uint64_t) { throw std::logic_error("Application snapshots are unsupported"); }

    // Durable RSMs restore this atomically with application data before constructing
    // TRequestProcessor. It must identify committed progress in the same Raft log.
    uint64_t LastAppliedIndex = 0;
};

struct TDummyRsm: public IRsm {
    void Validate(TMessageHolder<TCommandRequest> message) const override;
    TMessageHolder<TMessage> Read(TMessageHolder<TCommandRequest> message, uint64_t index) override;
    TMessageHolder<TMessage> Write(TMessageHolder<TLogEntry> message, uint64_t index) override;
    TMessageHolder<TLogEntry> Prepare(TMessageHolder<TCommandRequest> message) override;

private:
    std::vector<TMessageHolder<TLogEntry>> Log;
};

using TNodeDict = std::unordered_map<uint32_t, std::shared_ptr<INode>>;

struct TVolatileState {
    struct TAppendRpc { uint64_t Seqno, LastIndex; };
    static constexpr size_t MaxPendingAppendRpcs = 256;
    // Distinct snapshot images retained by active peer transfers. New
    // compaction is deferred when this many generations are already pinned.
    static constexpr size_t MaxPinnedSnapshotGenerations = 2;
    uint64_t CommitIndex = 0;
    // Successful current-term RPC acknowledgements, separately from log indices.
    std::unordered_map<uint32_t, uint64_t> ReadAckSeqno;
    std::unordered_map<uint32_t, uint64_t> LastSentSeqno;
    std::unordered_map<uint32_t, std::deque<TAppendRpc>> PendingAppendRpcs;
    struct TSnapshotRpc {
        // Pinned for this transfer even if the local store compacts again.
        std::shared_ptr<const TSnapshot> Snapshot;
        std::shared_ptr<const std::string> Bytes;
        // Seqno identifies the outstanding chunk across retries; zero means
        // its reply was consumed and the next send needs a fresh identity.
        uint64_t Offset = 0, Seqno = 0, SentEnd = 0;
    };
    std::unordered_map<uint32_t, TSnapshotRpc> SnapshotRpcs;
    uint32_t LeaderId = 0;
    std::unordered_map<uint32_t, uint64_t> NextIndex;
    std::unordered_map<uint32_t, uint64_t> MatchIndex;
    std::unordered_set<uint32_t> Votes;
    std::unordered_map<uint32_t, ITimeSource::Time> HeartbeatDue;
    std::unordered_map<uint32_t, ITimeSource::Time> RpcDue;
    std::unordered_map<uint32_t, int> BatchSize;
    std::unordered_map<uint32_t, int> BackOff;
    ITimeSource::Time ElectionDue;

    std::vector<uint64_t> Indices;

    TVolatileState& Vote(uint32_t id);
    // Compatibility helper for all-voter fixtures; mixed roles require IDs below.
    TVolatileState& CommitAdvance(int nservers, const IState& state);
    TVolatileState& CommitAdvance(const std::vector<uint32_t>& voters, uint32_t self, const IState& state);
    TVolatileState& CommitAdvance(const TQuorum& quorum, uint32_t self, const IState& state);
    TVolatileState& SetCommitIndex(uint64_t index);
    TVolatileState& SetElectionDue(ITimeSource::Time);
    TVolatileState& SetNextIndex(uint32_t id, uint64_t nextIndex);
    TVolatileState& SetMatchIndex(uint32_t id, uint64_t matchIndex);
    TVolatileState& SetHearbeatDue(uint32_t id, ITimeSource::Time heartbeatDue);
    TVolatileState& SetRpcDue(uint32_t id, ITimeSource::Time rpcDue);
    TVolatileState& SetBatchSize(uint32_t id, int size);
    TVolatileState& SetBackOff(uint32_t id, int size);
    TVolatileState& SetLeaderId(uint32_t id);

    bool operator==(const TVolatileState& other) const {
        return CommitIndex == other.CommitIndex &&
            NextIndex == other.NextIndex &&
            MatchIndex == other.MatchIndex;
    }
};

enum class EState: int {
    NONE = 0,
    CANDIDATE = 1,
    FOLLOWER = 2,
    LEADER = 3,
};

class TRaft {
public:
    // Nodes contains every replication peer; learners may include the local ID.
    // Voter roles remain fixed; learner replication targets can be staged locally.
    TRaft(std::shared_ptr<IState> state, int node, const TNodeDict& nodes, std::vector<uint32_t> learners = {});

    void Process(ITimeSource::Time now, TMessageHolder<TMessage> message, const std::shared_ptr<INode>& replyTo = {});
    void ProcessTimeout(ITimeSource::Time now);

    // Local proposals require leader state. Role rejection leaves storage and
    // the supplied entry unchanged; followers receive entries through Process.
    uint64_t Append(TMessageHolder<TLogEntry> entry);
    uint32_t GetLeaderId() const;
    uint64_t GetLastIndex() const;
    std::vector<uint32_t> GetVoterIds() const;
    std::vector<uint32_t> GetLearnerIds() const { return Learners; }
    bool IsVoter(uint32_t id) const;
    // Leader-only, point-in-time log catch-up; not permission to promote.
    bool IsLearnerCaughtUp(uint32_t id) const;
    // Leader-local replication staging; not a membership change. Caller owns
    // endpoint lifecycle, reconciles targets after failover/restart, and must
    // serialize these calls with all Raft processing.
    void AddLearnerTarget(uint32_t id, std::shared_ptr<INode> node);
    std::shared_ptr<INode> RemoveLearnerTarget(uint32_t id);
    // Capture applied state and atomically replace its prefix with a snapshot.
    // Throws without mutation while two distinct generations remain pinned by
    // outgoing peer transfers; callers may retry after a transfer completes.
    TSnapshot Compact();
    uint64_t ApproveRead();
    // Highest barrier confirmed by a fresh majority in this leadership term.
    // Zero means no confirmation (including before a current-term commit).
    uint64_t CommitSeqno() const;

// ut
    const auto& GetState() const {
        return State;
    }

    EState CurrentStateName() const {
        return StateName;
    }

    void Become(EState newStateName);

    const TVolatileState* GetVolatileState() const {
        return VolatileState.get();
    }

    const uint32_t GetId() const {
        return Id;
    }

    int GetMinVotes() const {
        return MinVotes;
    }

    int GetNpeers() const {
        return Npeers;
    }

    int GetNservers() const {
        // Voting members only; GetNpeers counts all remote replication targets.
        return Nservers;
    }

private:
    friend class TRequestProcessor;
    void RestoreAppliedIndex(uint64_t index);
    void AdvanceCommitIndex(uint64_t index);
    void AdvanceLeaderCommit();
    void OnInstallSnapshot(ITimeSource::Time now, TMessageHolder<TInstallSnapshotRequest> message);
    void OnInstallSnapshot(TMessageHolder<TInstallSnapshotResponse> message);
    TMessageHolder<TInstallSnapshotRequest> CreateInstallSnapshot(uint32_t nodeId);
    void Candidate(ITimeSource::Time now, TMessageHolder<TMessage> message);
    void Follower(ITimeSource::Time now, TMessageHolder<TMessage> message);
    void Leader(ITimeSource::Time now, TMessageHolder<TMessage> message);

    void OnRequestVote(ITimeSource::Time now, TMessageHolder<TRequestVoteRequest> message);
    void OnRequestVote(TMessageHolder<TRequestVoteResponse> message);
    void OnAppendEntries(ITimeSource::Time now, TMessageHolder<TAppendEntriesRequest> message);
    void OnAppendEntries(TMessageHolder<TAppendEntriesResponse> message);

    void LeaderTimeout(ITimeSource::Time now);
    void CandidateTimeout(ITimeSource::Time now);
    void FollowerTimeout(ITimeSource::Time now);

    TMessageHolder<TRequestVoteRequest> CreateVote(uint32_t nodeId);
    TMessageHolder<TAppendEntriesRequest> CreateAppendEntries(uint32_t nodeId);
    ITimeSource::Time MakeElection(ITimeSource::Time now);
    uint64_t NextSeqno();
    bool ValidateRpc(TMessageHolder<TMessage> message) const;

    uint32_t Id;
    TNodeDict Nodes;
    std::vector<uint32_t> Voters, Learners;
    std::optional<TQuorum> VotingQuorum;
    int MinVotes;
    int Npeers;
    int Nservers;
    std::shared_ptr<IState> State;
    std::unique_ptr<TVolatileState> VolatileState;
    uint64_t Seqno = 1; // zero is reserved for an unconfirmed read
    std::weak_ptr<IRsm> SnapshotApplication;
    struct TIncomingSnapshot {
        uint64_t Term, Index, BoundaryTerm, Total;
        uint32_t Leader;
        ITimeSource::Time LastProgress;
        std::string Bytes;
    };
    std::optional<TIncomingSnapshot> IncomingSnapshot;
    std::shared_ptr<const std::string> EncodedSnapshot;
    uint64_t EncodedSnapshotIndex = 0;

    EState StateName;
    uint32_t Seed = 31337;
};

class TRequestProcessor {
public:
    // Construct after restoring the RSM and before accepting network traffic.
    TRequestProcessor(std::shared_ptr<TRaft> raft, std::shared_ptr<IRsm> rsm, const TNodeDict& nodes);

    void CheckStateChange();
    void OnCommandRequest(TMessageHolder<TCommandRequest> message, const std::shared_ptr<INode>& replyTo);
    // Unattributed responses are ignored; pass the configured outbound peer.
    void OnCommandResponse(TMessageHolder<TCommandResponse> message, const std::shared_ptr<INode>& peer = {});
    void OnPeerDisconnected(const std::shared_ptr<INode>& peer);
    void ProcessCommitted();
    void ProcessWaiting();
    void CleanUp(const std::shared_ptr<INode>& replyTo);

private:
    void Forward(TMessageHolder<TCommandRequest> message, const std::shared_ptr<INode>& replyTo);
    void OnReadRequest(TMessageHolder<TCommandRequest> message, const std::shared_ptr<INode>& replyTo);
    void OnWriteRequest(TMessageHolder<TCommandRequest> message, const std::shared_ptr<INode>& replyTo);
    void PruneRequests(const std::shared_ptr<INode>& disconnected = {});
    void CancelForwarded(const std::shared_ptr<INode>& disconnected = {}, bool peerFailure = false);

    std::shared_ptr<TRaft> Raft;
    std::shared_ptr<IRsm> Rsm;
    TNodeDict Nodes;

    struct TWaiting {
        uint64_t Index;
        uint64_t Seqno = 0;
        TMessageHolder<TCommandRequest> Command;
        std::shared_ptr<INode> ReplyTo;
        // Zero is reserved for requests not owned by a leadership term.
        uint64_t Term = 0;
    };
    std::queue<TWaiting> Waiting;
    std::queue<TWaiting> StrongWaiting;
    std::queue<TWaiting> WaitingStateChange;

    struct TAnswer {
        uint64_t Term;
        TMessageHolder<TMessage> Reply;
    };
    // Retain application replies only for live local writes, matched by index
    // and term. An overwritten index must never redeem an older request.
    std::unordered_map<uint64_t, uint64_t> PendingWriteTerms;
    std::unordered_map<uint64_t, TAnswer> WriteAnswers;
    uint64_t RequestsTerm = 0;
    EState RequestsState = EState::NONE;
    uint32_t RequestsLeaderId = 0;
    struct TForwarded {
        std::shared_ptr<INode> Client, Peer;
        uint32_t OriginalCookie, LeaderId;
        uint64_t Term;
    };
    // Never reuse a wire cookie within this processor, including after failure.
    uint64_t NextForwardCookie = 1;
    std::unordered_map<uint32_t, TForwarded> Forwarded;
};

