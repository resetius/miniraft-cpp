#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <functional>

#include "messages.h"
#include "timesource.h"

struct INode {
    virtual ~INode() = default;
    virtual void Send(const TMessageHolder<TMessage>& message) = 0;
};

using TNodeDict = std::unordered_map<int, std::shared_ptr<INode>>;

class TNode: public INode {
public:
    TNode(int id, const std::string& host, int port)
        : Id_(id)
        , Host_(host)
        , Port_(port)
    { }

    void Send(const TMessageHolder<TMessage>& message) override;

private:
    int Id_;
    std::string Host_;
    int Port_;
};

struct TState {
    int CurrentTerm = 1;
    int VotedFor = 0;
    std::vector<TLogEntry> Log;

    int LogTerm(int index = -1) {
        if (index < 0) {
            index = Log.size();
        }
        if (index < 1 || index > Log.size()) {
            return 0;
        } else {
            return Log[index-1].Term;
        }
    }
};

struct TVolatileState {
    int CommitIndex = 0;
    int LastApplied = 0;
    std::vector<int> NextIndex;
    std::vector<int> MatchIndex;
    std::vector<bool> Votes;

    TVolatileState& SetVotes(std::vector<bool>& votes);
    TVolatileState& SetLastApplied(int index);
    TVolatileState& CommitAdvance(int nservers, int lastIndex, const TState& state);
    TVolatileState& SetCommitIndex(int index);
    TVolatileState& MergeNextIndex(const std::vector<int>& nextIndex);
    TVolatileState& MergeMatchIndex(const std::vector<int>& matchIndex);
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

    void Process(TMessageHolder<TMessage> message, INode* replyTo = nullptr);
    void ApplyResult(ITimeSource::Time now, std::unique_ptr<TResult> result, INode* replyTo = nullptr);

    EState CurrentStateName() const {
        return StateName;
    }

    void Become(EState newStateName);

private:
    std::unique_ptr<TResult> Follower(ITimeSource::Time now, TMessageHolder<TMessage> message);
    std::unique_ptr<TResult> Candidate(ITimeSource::Time now, TMessageHolder<TMessage> message);
    std::unique_ptr<TResult> Leader(ITimeSource::Time now, TMessageHolder<TMessage> message);

    std::unique_ptr<TResult> OnRequestVote(TMessageHolder<TRequestVoteRequest> message);
    std::unique_ptr<TResult> OnAppendEntries(TMessageHolder<TAppendEntriesRequest> message);
    TMessageHolder<TRequestVoteRequest> CreateVote();

    int Id;
    TNodeDict Nodes;
    std::shared_ptr<ITimeSource> TimeSource;
    int MinVotes;
    int Npeers;
    int Nservers;
    std::unique_ptr<TState> State;
    std::unique_ptr<TVolatileState> VolatileState;

    EState StateName;
    ITimeSource::Time LastTime;
};
