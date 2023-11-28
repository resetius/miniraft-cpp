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
    virtual void Send(const TMessageHolder<TMessage>& message) = 0;
    virtual void Drain() = 0;
};

using TNodeDict = std::unordered_map<int, std::shared_ptr<INode>>;

struct TState {
    uint64_t CurrentTerm = 1;
    uint32_t VotedFor = 0;
    std::vector<TMessageHolder<TLogEntry>> Log;

    int LogTerm(int index = -1) const {
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
    std::unordered_map<int, uint64_t> NextIndex;
    std::unordered_map<int, uint64_t> MatchIndex;
    std::unordered_set<int> Votes;

    TVolatileState& SetVotes(std::unordered_set<int>& votes);
    TVolatileState& SetLastApplied(int index);
    TVolatileState& CommitAdvance(int nservers, int lastIndex, const TState& state);
    TVolatileState& SetCommitIndex(int index);
    TVolatileState& MergeNextIndex(const std::unordered_map<int, uint64_t>& nextIndex);
    TVolatileState& MergeMatchIndex(const std::unordered_map<int, uint64_t>& matchIndex);
};

enum class EState: int {
    NONE = 0,
    CANDIDATE = 1,
    FOLLOWER = 2,
    LEADER = 3,
};

struct TResult {
    std::unique_ptr<TState> NextState;
    std::unique_ptr<TVolatileState> NextVolatileState;
    EState NextStateName = EState::NONE;
    bool UpdateLastTime = false;
    TMessageHolder<TMessage> Message;
    std::vector<TMessageHolder<TAppendEntriesRequest>> Messages;
};

class TRaft {
public:
    TRaft(int node, const TNodeDict& nodes, const std::shared_ptr<ITimeSource>& ts);

    void Process(TMessageHolder<TMessage> message, const std::shared_ptr<INode>& replyTo = {});
    void ApplyResult(ITimeSource::Time now, std::unique_ptr<TResult> result, const std::shared_ptr<INode>& replyTo = {});

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

    const auto GetLastTime() const {
        return LastTime;
    }

    const uint32_t GetId() const {
        return Id;
    }

    std::unique_ptr<TResult> Candidate(ITimeSource::Time now, TMessageHolder<TMessage> message);

private:
    std::unique_ptr<TResult> Follower(ITimeSource::Time now, TMessageHolder<TMessage> message);
    std::unique_ptr<TResult> Leader(ITimeSource::Time now, TMessageHolder<TMessage> message);

    std::unique_ptr<TResult> OnRequestVote(TMessageHolder<TRequestVoteRequest> message);
    std::unique_ptr<TResult> OnRequestVote(TMessageHolder<TRequestVoteResponse> message);
    std::unique_ptr<TResult> OnAppendEntries(TMessageHolder<TAppendEntriesRequest> message);
    std::unique_ptr<TResult> OnAppendEntries(TMessageHolder<TAppendEntriesResponse> message);
    TMessageHolder<TRequestVoteRequest> CreateVote();
    std::vector<TMessageHolder<TAppendEntriesRequest>> CreateAppendEntries();
    void ProcessWaiting();

    uint32_t Id;
    TNodeDict Nodes;
    std::shared_ptr<ITimeSource> TimeSource;
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
            return Index < other.Index;
        }
    };
    std::priority_queue<TWaiting> waiting;

    EState StateName;
    ITimeSource::Time LastTime;
};
