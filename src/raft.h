#pragma once

#include <memory>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "messages.h"
#include "timesource.h"

struct INode {
    virtual ~INode() = default;
    virtual void Send(TMessageHolder<TMessage> message) = 0;
    virtual void Drain() = 0;
};

// CommandRequest -> Write? -> LogEntry -> Append -> Committed As Index -> Applied As Index (Same) -> Index -> CommandResponse
// CommandRequest -> Read? -> CurrentIndex (fixate) >= CommittedIndex -> CommandResponse
struct IRsm {
    virtual ~IRsm() = default;
    virtual TMessageHolder<TMessage> Read(TMessageHolder<TCommandRequest> message) = 0;
    virtual void Write(TMessageHolder<TLogEntry> message) = 0;
    virtual TMessageHolder<TLogEntry> Prepare(TMessageHolder<TCommandRequest> message, uint64_t term) = 0;
};

struct TDummyRsm: public IRsm {
    TMessageHolder<TMessage> Read(TMessageHolder<TCommandRequest> message) override;
    void Write(TMessageHolder<TLogEntry> message) override;
    TMessageHolder<TLogEntry> Prepare(TMessageHolder<TCommandRequest> message, uint64_t term) override;
};

using TNodeDict = std::unordered_map<uint32_t, std::shared_ptr<INode>>;

struct TState {
    uint64_t CurrentTerm = 1;
    uint32_t VotedFor = 0;
    std::vector<TMessageHolder<TLogEntry>> Log;

    uint64_t LogTerm(int64_t index = -1) const {
        if (index < 0) {
            index = Log.size();
        }
        if (index < 1 || index > Log.size()) {
            return 0;
        } else {
            return Log[index-1]->Term;
        }
    }
};

struct TVolatileState {
    uint64_t CommitIndex = 0;
    uint64_t LastApplied = 0;
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
    TVolatileState& SetLastApplied(int index);
    TVolatileState& CommitAdvance(int nservers, const TState& state);
    TVolatileState& SetCommitIndex(int index);
    TVolatileState& SetElectionDue(ITimeSource::Time);
    TVolatileState& SetNextIndex(uint32_t id, uint64_t nextIndex);
    TVolatileState& SetMatchIndex(uint32_t id, uint64_t matchIndex);
    TVolatileState& SetHearbeatDue(uint32_t id, ITimeSource::Time heartbeatDue);
    TVolatileState& SetRpcDue(uint32_t id, ITimeSource::Time rpcDue);
    TVolatileState& SetBatchSize(uint32_t id, int size);
    TVolatileState& SetBackOff(uint32_t id, int size);
};

enum class EState: int {
    NONE = 0,
    CANDIDATE = 1,
    FOLLOWER = 2,
    LEADER = 3,
};

class TRaft {
public:
    TRaft(std::shared_ptr<IRsm> rsm, int node, const TNodeDict& nodes);

    void Process(ITimeSource::Time now, TMessageHolder<TMessage> message, const std::shared_ptr<INode>& replyTo = {});
    void ProcessTimeout(ITimeSource::Time now);

// ut
    EState CurrentStateName() const {
        return StateName;
    }

    void Become(EState newStateName);

    const TState* GetState() const {
        return State.get();
    }

    void SetState(const TState& state) {
        *State = state;
    }

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
    void Leader(ITimeSource::Time now, TMessageHolder<TMessage> message, const std::shared_ptr<INode>& replyTo);

    void OnRequestVote(ITimeSource::Time now, TMessageHolder<TRequestVoteRequest> message);
    void OnRequestVote(TMessageHolder<TRequestVoteResponse> message);
    void OnAppendEntries(ITimeSource::Time now, TMessageHolder<TAppendEntriesRequest> message);
    void OnAppendEntries(TMessageHolder<TAppendEntriesResponse> message);
    void OnCommandRequest(TMessageHolder<TCommandRequest> message, const std::shared_ptr<INode>& replyTo);

    void LeaderTimeout(ITimeSource::Time now);
    void CandidateTimeout(ITimeSource::Time now);
    void FollowerTimeout(ITimeSource::Time now);

    TMessageHolder<TRequestVoteRequest> CreateVote(uint32_t nodeId);
    TMessageHolder<TAppendEntriesRequest> CreateAppendEntries(uint32_t nodeId);
    void ProcessWaiting();
    ITimeSource::Time MakeElection(ITimeSource::Time now);

    std::shared_ptr<IRsm> Rsm;
    uint32_t Id;
    TNodeDict Nodes;
    int MinVotes;
    int Npeers;
    int Nservers;
    std::unique_ptr<TState> State;
    std::unique_ptr<TVolatileState> VolatileState;

    struct TWaiting {
        uint64_t Index;
        TMessageHolder<TMessage> Message;
        std::shared_ptr<INode> ReplyTo;
        bool operator< (const TWaiting& other) const {
            return Index > other.Index;
        }
    };
    std::priority_queue<TWaiting> waiting;

    EState StateName;
    uint32_t Seed = 31337;
};
