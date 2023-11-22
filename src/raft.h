#pragma once

#include <string>
#include <vector>

#include "messages.h"
#include "timesource.h"

class TNode {
public:
    TNode(int id, const std::string& host, int port)
        : Id_(id)
        , Host_(host)
        , Port_(port)
    { }

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

class Raft {
public:
    Raft(int node, const std::vector<TNode>& nodes, const TTimeSource& ts);

private:
    int Id;
    std::vector<TNode> Nodes;
    TTimeSource TimeSource;
    int MinVotes;
    int Npeers;
    int Nservers;
    TState State;
    TVolatileState VolatileState;
};
