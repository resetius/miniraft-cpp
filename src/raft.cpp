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

TMessageHolder<TMessage> TDummyRsm::Read(TMessageHolder<TCommandRequest> message)
{
    return {};
}

void TDummyRsm::Write(TMessageHolder<TLogEntry> message)
{ }

TMessageHolder<TLogEntry> TDummyRsm::Prepare(TMessageHolder<TCommandRequest> command, uint64_t term)
{
    auto dataSize = command->Len - sizeof(TCommandRequest);
    auto entry = NewHoldedMessage<TLogEntry>(sizeof(TLogEntry)+dataSize);
    memcpy(entry->Data, command->Data, dataSize);
    entry->Term = term;
    return entry;
}

TVolatileState& TVolatileState::SetElectionDue(ITimeSource::Time due) {
    ElectionDue = due;
    return *this;
}

TVolatileState& TVolatileState::Vote(uint32_t nodeId)
{
    Votes.insert(nodeId);
    return *this;
}

TVolatileState& TVolatileState::CommitAdvance(int nservers, const TState& state)
{
    auto lastIndex = state.Log.size();
    Indices.clear(); Indices.reserve(nservers);
    for (auto [_, index] : MatchIndex) {
        Indices.push_back(index);
    }
    Indices.push_back(lastIndex);
    while (Indices.size() < nservers) {
        Indices.push_back(0);
    }
    std::sort(Indices.begin(), Indices.end());
    auto commitIndex = std::max(CommitIndex, Indices[nservers / 2]);
    if (state.LogTerm(commitIndex) == state.CurrentTerm) {
        CommitIndex = commitIndex;
    }
    return *this;
}

TVolatileState& TVolatileState::SetNextIndex(uint32_t id, uint64_t nextIndex)
{
    NextIndex[id] = nextIndex;
    return *this;
}

TVolatileState& TVolatileState::SetMatchIndex(uint32_t id, uint64_t matchIndex)
{
    MatchIndex[id] = matchIndex;
    return *this;
}

TVolatileState& TVolatileState::SetHearbeatDue(uint32_t id, ITimeSource::Time heartbeatDue)
{
    HeartbeatDue[id] = heartbeatDue;
    return *this;
}

TVolatileState& TVolatileState::SetRpcDue(uint32_t id, ITimeSource::Time rpcDue)
{
    RpcDue[id] = rpcDue;
    return *this;
}

TVolatileState& TVolatileState::SetBatchSize(uint32_t id, int size)
{
    BatchSize[id] = size;
    return *this;
}

TVolatileState& TVolatileState::SetBackOff(uint32_t id, int size) {
    BackOff[id] = size;
    return *this;
}

TVolatileState& TVolatileState::SetCommitIndex(int index)
{
    CommitIndex = index;
    return *this;
}

TRaft::TRaft(std::shared_ptr<IRsm> rsm, int node, const TNodeDict& nodes)
    : Rsm(rsm)
    , Id(node)
    , Nodes(nodes)
    , MinVotes((nodes.size()+2+nodes.size()%2)/2)
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
        (*VolatileState)
            .Vote(message->Src)
            .SetRpcDue(message->Src, ITimeSource::Max);
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
        for (uint32_t i = 0 ; i < message.PayloadSize; i++) {
            auto& data = message.Payload[i];
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

    (*VolatileState)
        .SetCommitIndex(commitIndex)
        .SetElectionDue(MakeElection(now));
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
            .SetMatchIndex(nodeId, matchIndex)
            .SetNextIndex(nodeId, message->MatchIndex+1)
            .SetRpcDue(nodeId, ITimeSource::Time{})
            .SetBatchSize(nodeId, 1024)
            .SetBackOff(nodeId, 1)
            .CommitAdvance(Nservers, *State);
    } else {
        auto backOff = std::max(VolatileState->BackOff[nodeId], 1);
        auto nextIndex = VolatileState->NextIndex[nodeId] > backOff
            ? VolatileState->NextIndex[nodeId] - backOff
            : 0;
        (*VolatileState)
            .SetNextIndex(nodeId, std::max((uint64_t)1, nextIndex))
            .SetRpcDue(nodeId, ITimeSource::Time{})
            .SetBatchSize(nodeId, 1)
            .SetBackOff(nodeId, std::min(32768, backOff << 1));
    }
}

void TRaft::OnCommandRequest(TMessageHolder<TCommandRequest> command, const std::shared_ptr<INode>& replyTo) {
    auto& log = State->Log;
    auto entry = Rsm->Prepare(std::move(command), State->CurrentTerm);
    log.emplace_back(std::move(entry));
    auto index = log.size();
    if (replyTo) {
        auto mes = NewHoldedMessage(TCommandResponse {.Index = index});
        waiting.emplace(TWaiting{mes->Index, mes, replyTo});
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
    int batchSize = std::max(1, VolatileState->BatchSize[nodeId]);
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

    if (lastIndex - prevIndex > 0) {
        mes.InitPayload(lastIndex - prevIndex);
        uint32_t j = 0;
        for (auto i = prevIndex; i < lastIndex; i++) {
            mes.Payload[j++] = State->Log[i];
        }
        // std::cout << "Send " << j << " entries to " << nodeId << "\n";
    }
    return mes;
}

void TRaft::Follower(ITimeSource::Time now, TMessageHolder<TMessage> message) {
    if (auto maybeRequestVote = message.Maybe<TRequestVoteRequest>()) {
        OnRequestVote(now, std::move(maybeRequestVote.Cast()));
    } else if (auto maybeAppendEntries = message.Maybe<TAppendEntriesRequest>()) {
        OnAppendEntries(now, std::move(maybeAppendEntries.Cast()));
    }
}

void TRaft::Candidate(ITimeSource::Time now, TMessageHolder<TMessage> message) {
    if (auto maybeResponseVote = message.Maybe<TRequestVoteResponse>()) {
        OnRequestVote(std::move(maybeResponseVote.Cast()));
    } else if (auto maybeRequestVote = message.Maybe<TRequestVoteRequest>()) {
        OnRequestVote(now, std::move(maybeRequestVote.Cast()));
    } else if (auto maybeAppendEntries = message.Maybe<TAppendEntriesRequest>()) {
        OnAppendEntries(now, std::move(maybeAppendEntries.Cast()));
    }
}

void TRaft::Leader(ITimeSource::Time now, TMessageHolder<TMessage> message, const std::shared_ptr<INode>& replyTo) {
    if (auto maybeAppendEntries = message.Maybe<TAppendEntriesResponse>()) {
        OnAppendEntries(std::move(maybeAppendEntries.Cast()));
    } else if (auto maybeCommandRequest = message.Maybe<TCommandRequest>()) {
        OnCommandRequest(std::move(maybeCommandRequest.Cast()), replyTo);
    } else if (auto maybeVoteRequest = message.Maybe<TRequestVoteRequest>()) {
        OnRequestVote(now, std::move(maybeVoteRequest.Cast()));
    } else if (auto maybeAppendEntries = message.Maybe<TAppendEntriesRequest>()) {
        OnAppendEntries(now, std::move(maybeAppendEntries.Cast()));
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

void TRaft::ProcessCommitted() {
    auto commitIndex = VolatileState->CommitIndex;
    if (commitIndex > 0) {
        for (auto i = VolatileState->LastApplied; i <= commitIndex; i++) {
            Rsm->Write(State->Log[i-1]);
        }
        VolatileState->LastApplied = commitIndex;
    }
}

void TRaft::ProcessWaiting() {
    auto lastApplied = VolatileState->LastApplied;
    while (!waiting.empty() && waiting.top().Index <= lastApplied) {
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

    ProcessCommitted();
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
            nextVolatileState->CommitIndex = VolatileState->CommitIndex;
            VolatileState = std::move(nextVolatileState);
            State->VotedFor = Id;
            State->CurrentTerm ++;
            Become(EState::CANDIDATE);
        }
    }

    if (StateName == EState::CANDIDATE) {
        int nvotes = VolatileState->Votes.size()+1;
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
