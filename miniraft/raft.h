#pragma once

#include <memory>
#include <queue>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "messages.h"
#include "timesource.h"
#include "state.h"

struct INode {
    virtual ~INode() = default;
    virtual void Send(TMessageHolder<TMessage> message) = 0;
    virtual void Drain() = 0;
};

// CommandRequest -> Write? -> LogEntry -> Append -> Committed As Index -> Applied As Index (Same) -> Index -> CommandResponse
// CommandRequest -> Read? -> CurrentIndex (fixate) >= CommittedIndex -> CommandResponse
struct IRsm {
    virtual ~IRsm() = default;
    virtual TMessageHolder<TMessage> Read(TMessageHolder<TCommandRequest> message, uint64_t index) = 0;
    virtual TMessageHolder<TMessage> Write(TMessageHolder<TLogEntry> message, uint64_t index) = 0;
    virtual TMessageHolder<TLogEntry> Prepare(TMessageHolder<TCommandRequest> message) = 0;

    uint64_t LastAppliedIndex = 0;
};

struct TDummyRsm: public IRsm {
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
    uint64_t CommitIndex = 0;
    // Successful current-term RPC acknowledgements, separately from log indices.
    std::unordered_map<uint32_t, uint64_t> ReadAckSeqno;
    std::unordered_map<uint32_t, uint64_t> LastSentSeqno;
    std::unordered_map<uint32_t, std::deque<TAppendRpc>> PendingAppendRpcs;
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
    TVolatileState& CommitAdvance(int nservers, const IState& state);
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
    TRaft(std::shared_ptr<IState> state, int node, const TNodeDict& nodes);

    void Process(ITimeSource::Time now, TMessageHolder<TMessage> message, const std::shared_ptr<INode>& replyTo = {});
    void ProcessTimeout(ITimeSource::Time now);

    uint64_t Append(TMessageHolder<TLogEntry> entry);
    uint32_t GetLeaderId() const;
    uint64_t GetLastIndex() const;
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
        return Nservers;
    }

private:
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
    int MinVotes;
    int Npeers;
    int Nservers;
    std::shared_ptr<IState> State;
    std::unique_ptr<TVolatileState> VolatileState;
    uint64_t Seqno = 1; // zero is reserved for an unconfirmed read

    EState StateName;
    uint32_t Seed = 31337;
};

class TRequestProcessor {
public:
    TRequestProcessor(std::shared_ptr<TRaft> raft, std::shared_ptr<IRsm> rsm, const TNodeDict& nodes)
        : Raft(raft)
        , Rsm(rsm)
        , Nodes(nodes)
    { }

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

