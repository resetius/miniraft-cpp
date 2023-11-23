#pragma once

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

using TStateFunc = std::function<void(uint64_t now, const TMessageHolder<TMessage>& message)>;

struct TResult {
    TState NextState;
    TVolatileState NextVolatileState;
    TStateFunc NextStateFunc;
    bool UpdateLastTime;
    TMessageHolder<TMessage> Message;
    std::vector<TMessageHolder<TMessage>> Messages;
};

class TRaft {
public:
    TRaft(int node, const std::vector<TNode>& nodes, const TTimeSource& ts);

    void process(const TMessageHolder<TMessage>& message, TNode* replyTo = nullptr);
    void applyResult(uint64_t now, const TResult& result, TNode* replyTo = nullptr);

private:
    void follower(uint64_t now, const TMessageHolder<TMessage>& message);

    int Id;
    std::vector<TNode> Nodes;
    TTimeSource TimeSource;
    int MinVotes;
    int Npeers;
    int Nservers;
    TState State;
    TVolatileState VolatileState;

    TStateFunc StateFunc;
    uint64_t LastTime;
};
