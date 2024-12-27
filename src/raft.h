#pragma once

#include <memory>
#include <queue>
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
    uint64_t CommitIndex = 0;
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
    TVolatileState& SetCommitIndex(int index);
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

    uint32_t Id;
    TNodeDict Nodes;
    int MinVotes;
    int Npeers;
    int Nservers;
    std::shared_ptr<IState> State;
    std::unique_ptr<TVolatileState> VolatileState;

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
    void OnCommandResponse(TMessageHolder<TCommandResponse> message);
    void ProcessCommitted();
    void ProcessWaiting();
    void CleanUp(const std::shared_ptr<INode>& replyTo);

private:
    void Forward(TMessageHolder<TCommandRequest> message, const std::shared_ptr<INode>& replyTo);
    void OnReadRequest(TMessageHolder<TCommandRequest> message, const std::shared_ptr<INode>& replyTo);
    void OnWriteRequest(TMessageHolder<TCommandRequest> message, const std::shared_ptr<INode>& replyTo);

    std::shared_ptr<TRaft> Raft;
    std::shared_ptr<IRsm> Rsm;
    TNodeDict Nodes;

    struct TWaiting {
        uint64_t Index;
        TMessageHolder<TCommandRequest> Command;
        std::shared_ptr<INode> ReplyTo;
    };
    std::queue<TWaiting> Waiting;
    std::queue<TWaiting> WaitingStateChange;

    struct TAnswer {
        uint64_t Index;
        TMessageHolder<TMessage> Reply;
    };
    std::queue<TAnswer> WriteAnswers;
    uint32_t ForwardCookie = 1;
    std::unordered_map<uint32_t, std::shared_ptr<INode>> Cookie2Client;
    std::unordered_map<std::shared_ptr<INode>, std::unordered_set<uint32_t>> Client2Cookie;
};

