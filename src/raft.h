#pragma once

#include <memory>
#include <string>
#include <vector>
#include <functional>

#include "messages.h"
#include "timesource.h"

class TNode {
public:
    TNode(int id, const std::string& host, int port)
        : Id_(id)
        , Host_(host)
        , Port_(port)
    { }

    void send(const TMessageHolder<TMessage>& message);

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

struct TResult;

using TStateFunc = std::function<std::unique_ptr<TResult>(uint64_t now, const TMessageHolder<TMessage>& message)>;

struct TResult {
    std::unique_ptr<TState> NextState;
    std::unique_ptr<TVolatileState> NextVolatileState;
    TStateFunc NextStateFunc;
    bool UpdateLastTime;
    TMessageHolder<TMessage> Message;
    std::vector<TMessageHolder<TAppendEntriesRequest>> Messages;
};

class TRaft {
public:
    TRaft(int node, const std::vector<TNode>& nodes, const TTimeSource& ts);

    void process(const TMessageHolder<TMessage>& message, TNode* replyTo = nullptr);
    void applyResult(uint64_t now, std::unique_ptr<TResult> result, TNode* replyTo = nullptr);

private:
    std::unique_ptr<TResult> follower(uint64_t now, const TMessageHolder<TMessage>& message);

    int Id;
    std::vector<TNode> Nodes;
    TTimeSource TimeSource;
    int MinVotes;
    int Npeers;
    int Nservers;
    std::unique_ptr<TState> State;
    std::unique_ptr<TVolatileState> VolatileState;

    TStateFunc StateFunc;
    uint64_t LastTime;
};
