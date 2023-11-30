#include <chrono>
#include <climits>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <iostream>
#include <algorithm>

#include <string.h>
#include <assert.h>

#include "raft.h"
#include "messages.h"
#include "timesource.h"

namespace {

static uint32_t rand_(uint32_t* seed) {
    *seed ^= *seed << 13;
    *seed ^= *seed >> 17;
    *seed ^= *seed << 5;
    return *seed;
}

} // namespace

TVolatileState& TVolatileState::SetVotes(std::unordered_set<uint32_t>& votes)
{
    Votes = votes;
    return *this;
}

TVolatileState& TVolatileState::CommitAdvance(int nservers, int lastIndex, const TState& state)
{
    std::vector<uint64_t> indices; indices.reserve(MatchIndex.size()+1+nservers);
    for (auto [_, index] : MatchIndex) {
        indices.push_back(index);
    }
    indices.push_back(lastIndex);
    while (indices.size() < nservers) {
        indices.push_back(0);
    }
    std::sort(indices.begin(), indices.end());
    auto commitIndex = std::max(CommitIndex, indices[nservers / 2]);
    if (state.LogTerm(commitIndex) == state.CurrentTerm) {
        CommitIndex = commitIndex;
    }
    return *this;
}

TVolatileState& TVolatileState::MergeNextIndex(const std::unordered_map<int, uint64_t>& nextIndex)
{
    for (const auto& [k, v] : nextIndex) {
        NextIndex[k] = v;
    }
    return *this;
}

TVolatileState& TVolatileState::MergeMatchIndex(const std::unordered_map<int, uint64_t>& matchIndex)
{
    for (const auto& [k, v] : matchIndex) {
        MatchIndex[k] = v;
    }
    return *this;
}

TVolatileState& TVolatileState::MergeHearbeatDue(const std::unordered_map<int, ITimeSource::Time>& heartbeatDue)
{
    for (const auto& [k, v] : heartbeatDue) {
        HeartbeatDue[k] = v;
    }
    return *this;
}

TVolatileState& TVolatileState::MergeRpcDue(const std::unordered_map<int, ITimeSource::Time>& rpcDue)
{
    for (const auto& [k, v] : rpcDue) {
        RpcDue[k] = v;
    }
    return *this;
}

TVolatileState& TVolatileState::SetCommitIndex(int index)
{
    CommitIndex = index;
    return *this;
}

TRaft::TRaft(int node, const TNodeDict& nodes)
    : Id(node)
    , Nodes(nodes)
    , MinVotes((nodes.size()+2)/2)
    , Npeers(nodes.size())
    , Nservers(nodes.size()+1)
    , State(std::make_unique<TState>())
    , VolatileState(std::make_unique<TVolatileState>())
    , StateName(EState::FOLLOWER)
{
    for (auto [id, _] : Nodes) {
        VolatileState->NextIndex[id] = 1;
    }
}

void TRaft::OnRequestVote(ITimeSource::Time now, TMessageHolder<TRequestVoteRequest> message) {
    if (message->Term < State->CurrentTerm) {
        auto reply = NewHoldedMessage(
            TMessageEx {.Src = Id, .Dst = message->Src, .Term = State->CurrentTerm},
            TRequestVoteResponse {.VoteGranted = false});
        Nodes[reply->Dst]->Send(std::move(reply));
    } else if (message->Term == State->CurrentTerm) {
        bool accept = false;
        if (State->VotedFor == 0 || State->VotedFor == message->CandidateId) {
            if (message->LastLogTerm > State->LogTerm()) {
                accept = true;
            } else if (message->LastLogTerm == State->LogTerm() && message->LastLogIndex >= State->Log.size()) {
                accept = true;
            }
        }

        auto reply = NewHoldedMessage(
            TMessageEx {.Src = Id, .Dst = message->Src, .Term = State->CurrentTerm},
            TRequestVoteResponse {.VoteGranted = accept});

        if (accept) {
            VolatileState->ElectionDue = MakeElection(now);
            State->VotedFor = message->CandidateId;
        }

        Nodes[reply->Dst]->Send(std::move(reply));
    }
}

void TRaft::OnRequestVote(TMessageHolder<TRequestVoteResponse> message) {
    if (message->VoteGranted && message->Term == State->CurrentTerm) {
        auto votes = VolatileState->Votes;
        votes.insert(message->Src);

        (*VolatileState)
            .SetVotes(votes)
            .MergeRpcDue({{message->Src, ITimeSource::Max}});
    }
}

void TRaft::OnAppendEntries(ITimeSource::Time now, TMessageHolder<TAppendEntriesRequest> message) {
    if (message->Term < State->CurrentTerm) {
        VolatileState->ElectionDue = MakeElection(now);

        auto reply = NewHoldedMessage(
            TMessageEx {
                .Src = Id,
                .Dst = message->Src,
                .Term = State->CurrentTerm,
            },
            TAppendEntriesResponse {
                .MatchIndex = 0,
                .Success = false,
            });
        Nodes[reply->Dst]->Send(std::move(reply));
        return;
    }

    assert(message->Term == State->CurrentTerm);

    uint64_t matchIndex = 0;
    uint64_t commitIndex = VolatileState->CommitIndex;
    bool success = false;
    if (message->PrevLogIndex == 0 ||
        (message->PrevLogIndex <= State->Log.size()
            && State->LogTerm(message->PrevLogIndex) == message->PrevLogTerm))
    {
        success = true;
        auto index = message->PrevLogIndex;
        auto& log = State->Log;
        for (auto& data : message.Payload) {
            auto entry = data.Cast<TLogEntry>();
            index++;
            // replace or append log entries
            if (State->LogTerm(index) != entry->Term) {
                while (log.size() > index-1) {
                    log.pop_back();
                }
                log.push_back(entry);
            }
        }

        matchIndex = index;
        commitIndex = std::max(commitIndex, message->LeaderCommit);
    }

    auto reply = NewHoldedMessage(
        TMessageEx {.Src = Id, .Dst = message->Src, .Term = State->CurrentTerm},
        TAppendEntriesResponse {.MatchIndex = matchIndex, .Success = success});

    VolatileState->SetCommitIndex(commitIndex);
    VolatileState->ElectionDue = MakeElection(now);
    Become(EState::FOLLOWER);
    Nodes[reply->Dst]->Send(std::move(reply));
}

void TRaft::OnAppendEntries(TMessageHolder<TAppendEntriesResponse> message) {
    if (message->Term != State->CurrentTerm) {
        return;
    }

    auto nodeId = message->Src;
    if (message->Success) {
        auto matchIndex = std::max(VolatileState->MatchIndex[nodeId], message->MatchIndex);
        (*VolatileState)
            .MergeMatchIndex({{nodeId, matchIndex}})
            .MergeNextIndex({{nodeId, message->MatchIndex+1}})
            .CommitAdvance(Nservers, State->Log.size(), *State)
            .MergeRpcDue({{nodeId, ITimeSource::Time{}}});
    } else {
        (*VolatileState)
            .MergeNextIndex({{nodeId, std::max((uint64_t)1, VolatileState->NextIndex[nodeId]-1)}})
            //.MergeNextIndex({{nodeId, 1}})
            .MergeRpcDue({{nodeId, ITimeSource::Time{}}});
    }
}

TMessageHolder<TRequestVoteRequest> TRaft::CreateVote(uint32_t nodeId) {
    auto mes = NewHoldedMessage(
        TMessageEx {.Src = Id, .Dst = nodeId, .Term = State->CurrentTerm},
        TRequestVoteRequest {
            .LastLogIndex = State->Log.size(),
            .LastLogTerm = State->Log.empty() ? 0 : State->Log.back()->Term,
            .CandidateId = Id,
        });
    return mes;
}

TMessageHolder<TAppendEntriesRequest> TRaft::CreateAppendEntries(uint32_t nodeId) {
    static constexpr int batchSize = 128;
    auto prevIndex = VolatileState->NextIndex[nodeId] - 1;
    auto lastIndex = std::min(prevIndex+batchSize, (uint64_t)State->Log.size());
    if (VolatileState->MatchIndex[nodeId]+1 < VolatileState->NextIndex[nodeId]) {
        lastIndex = prevIndex;
    }

    auto mes = NewHoldedMessage(
        TMessageEx {.Src = Id, .Dst = nodeId, .Term = State->CurrentTerm},
        TAppendEntriesRequest {
            .PrevLogIndex = prevIndex,
            .PrevLogTerm = State->LogTerm(prevIndex),
            .LeaderCommit = std::min(VolatileState->CommitIndex, lastIndex),
            .LeaderId = Id,
            .Nentries = static_cast<uint32_t>(lastIndex - prevIndex),
        });
    std::vector<TMessageHolder<TMessage>> payload;
    payload.reserve(lastIndex - prevIndex);
    for (auto i = prevIndex; i < lastIndex; i++) {
        payload.push_back(State->Log[i]);
    }
    if (!payload.empty()) {
        std::cout << "Send " << payload.size() << " entries to " << nodeId << "\n";
    }
    mes.Payload = std::move(payload);
    return mes;
}

std::vector<TMessageHolder<TAppendEntriesRequest>> TRaft::CreateAppendEntries() {
    std::vector<TMessageHolder<TAppendEntriesRequest>> res;
    for (auto [nodeId, _] : Nodes) {
        res.emplace_back(CreateAppendEntries(nodeId));
    }
    return res;
}

void TRaft::Follower(ITimeSource::Time now, TMessageHolder<TMessage> message) {
    if (auto maybeRequestVote = message.Maybe<TRequestVoteRequest>()) {
        OnRequestVote(now, std::move(maybeRequestVote.Cast()));
    } else if (auto maybeAppendEntries = message.Maybe<TAppendEntriesRequest>()) {
        OnAppendEntries(now, maybeAppendEntries.Cast());
    }
}

void TRaft::Candidate(ITimeSource::Time now, TMessageHolder<TMessage> message) {
    if (auto maybeResponseVote = message.Maybe<TRequestVoteResponse>()) {
        OnRequestVote(std::move(maybeResponseVote.Cast()));
    } else if (auto maybeRequestVote = message.Maybe<TRequestVoteRequest>()) {
        OnRequestVote(now, std::move(maybeRequestVote.Cast()));
    } else if (auto maybeAppendEntries = message.Maybe<TAppendEntriesRequest>()) {
        OnAppendEntries(now, maybeAppendEntries.Cast());
    }
}

void TRaft::Leader(ITimeSource::Time now, TMessageHolder<TMessage> message, const std::shared_ptr<INode>& replyTo) {
    if (auto maybeAppendEntries = message.Maybe<TAppendEntriesResponse>()) {
        OnAppendEntries(maybeAppendEntries.Cast());
    } else if (auto maybeCommandRequest = message.Maybe<TCommandRequest>()) {
        auto command = maybeCommandRequest.Cast();
        auto& log = State->Log;
        auto dataSize = command->Len - sizeof(TCommandRequest);
        auto entry = NewHoldedMessage<TLogEntry>(sizeof(TLogEntry)+dataSize);
        memcpy(entry->Data, command->Data, dataSize);
        entry->Term = State->CurrentTerm;
        log.push_back(entry);
        auto index = log.size()-1;
        if (replyTo) {
            auto mes = NewHoldedMessage(TCommandResponse {.Index = index});
            waiting.emplace(TWaiting{mes->Index, mes, replyTo});
        }
    } else if (auto maybeVoteRequest = message.Maybe<TRequestVoteRequest>()) {
        OnRequestVote(now, std::move(maybeVoteRequest.Cast()));
    } else if (auto maybeVoteResponse = message.Maybe<TRequestVoteResponse>()) {
        // skip additional votes
    } else if (auto maybeAppendEntries = message.Maybe<TAppendEntriesRequest>()) {
        OnAppendEntries(now, maybeAppendEntries.Cast());
    }
}

void TRaft::Become(EState newStateName) {
    if (StateName != newStateName) {
        StateName = newStateName;
    }
}

void TRaft::Process(ITimeSource::Time now, TMessageHolder<TMessage> message, const std::shared_ptr<INode>& replyTo) {
    if (message.IsEx()) {
        auto messageEx = message.Cast<TMessageEx>();
        if (messageEx->Term > State->CurrentTerm) {
            State->CurrentTerm = messageEx->Term;
            State->VotedFor = 0;
            StateName = EState::FOLLOWER;
            if (VolatileState->ElectionDue <= now || VolatileState->ElectionDue == ITimeSource::Max) {
                VolatileState->ElectionDue = MakeElection(now);
            }
        }
    }
    switch (StateName) {
    case EState::FOLLOWER:
        Follower(now, std::move(message));
        break;
    case EState::CANDIDATE:
        Candidate(now, std::move(message));
        break;
    case EState::LEADER:
        Leader(now, std::move(message), replyTo);
        break;
    default:
        throw std::logic_error("Unknown state");
    }
}

void TRaft::ProcessWaiting() {
    auto commitIndex = VolatileState->CommitIndex;
    while (!waiting.empty() && waiting.top().Index <= commitIndex) {
        auto w = waiting.top(); waiting.pop();
        w.ReplyTo->Send(std::move(w.Message));
    }
}

void TRaft::FollowerTimeout(ITimeSource::Time now) {
    if (VolatileState->ElectionDue <= now) {
        Become(EState::CANDIDATE);
    }
}

void TRaft::CandidateTimeout(ITimeSource::Time now) {
    for (auto& [id, node] : Nodes) {
        if (VolatileState->RpcDue[id] <= now) {
            VolatileState->RpcDue[id] = now + TTimeout::Rpc;
            node->Send(CreateVote(id));
        }
    }
}

void TRaft::LeaderTimeout(ITimeSource::Time now) {
    for (auto& [id, node] : Nodes) {
        if (VolatileState->HeartbeatDue[id] <= now
            || (VolatileState->NextIndex[id] <= State->Log.size() &&
            VolatileState->RpcDue[id] <= now))
        {
            VolatileState->HeartbeatDue[id] = now + TTimeout::Election / 2;
            VolatileState->RpcDue[id] = now + TTimeout::Rpc;
            node->Send(CreateAppendEntries(id));
        }
    }

    ProcessWaiting();
}

void TRaft::ProcessTimeout(ITimeSource::Time now) {
    if (StateName == EState::CANDIDATE || StateName == EState::FOLLOWER) {
        if (VolatileState->ElectionDue <= now) {
            auto nextVolatileState = std::make_unique<TVolatileState>();
            for (auto [id, _] : Nodes) {
                nextVolatileState->NextIndex[id] = 1;
            }
            nextVolatileState->ElectionDue = MakeElection(now);
            VolatileState = std::move(nextVolatileState);
            State->VotedFor = Id;
            State->CurrentTerm ++;
            Become(EState::CANDIDATE);
        }
    }

    if (StateName == EState::CANDIDATE) {
        int nvotes = VolatileState->Votes.size()+1;
        std::cout << "Need/total: " << MinVotes << "/" << nvotes << "\n";
        if (nvotes >= MinVotes) {
            auto value = State->Log.size()+1;
            decltype(VolatileState->NextIndex) nextIndex;
            decltype(VolatileState->RpcDue) rpcDue;
            for (auto [id, _] : Nodes) {
                nextIndex.emplace(id, value);
                rpcDue.emplace(id, ITimeSource::Max);
            }

            auto nextVolatileState = std::make_unique<TVolatileState>(TVolatileState {
                .CommitIndex = VolatileState->CommitIndex,
                .LastApplied = VolatileState->LastApplied,
                .NextIndex = nextIndex,
                .RpcDue = rpcDue,
                .ElectionDue = ITimeSource::Max,
            });

            VolatileState = std::move(nextVolatileState);
            StateName = EState::LEADER;
        }
    }

    switch (StateName) {
    case EState::FOLLOWER:
        FollowerTimeout(now); break;
    case EState::CANDIDATE:
        CandidateTimeout(now); break;
    case EState::LEADER:
        LeaderTimeout(now); break;
    default:
        throw std::logic_error("Unknown state");
    }
}

ITimeSource::Time TRaft::MakeElection(ITimeSource::Time now) {
    uint64_t delta = (uint64_t)((1.0 + (double)rand_(&Seed) / (double)UINT_MAX) * TTimeout::Election.count());
    return now + std::chrono::milliseconds(delta);
}
