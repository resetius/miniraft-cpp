#include <cstdint>
#include <memory>
#include <stdexcept>
#include <iostream>

#include <string.h>
#include <assert.h>

#include "raft.h"
#include "messages.h"

TVolatileState& TVolatileState::SetVotes(std::unordered_set<int>& votes)
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

TVolatileState& TVolatileState::SetCommitIndex(int index)
{
    CommitIndex = index;
    return *this;
}

TRaft::TRaft(int node, const TNodeDict& nodes, const std::shared_ptr<ITimeSource>& ts)
    : Id(node)
    , Nodes(nodes)
    , TimeSource(ts)
    , MinVotes((nodes.size()+1/2))
    , Npeers(nodes.size())
    , Nservers(nodes.size()+1)
    , StateName(EState::FOLLOWER)
    , LastTime(TimeSource->Now())
{ }

std::unique_ptr<TResult> TRaft::OnRequestVote(TMessageHolder<TRequestVoteRequest> message) {
    if (message->Term < State->CurrentTerm) {
        auto reply = NewHoldedMessage<TRequestVoteResponse>(static_cast<uint32_t>(EMessageType::REQUEST_VOTE_RESPONSE), sizeof(TRequestVoteResponse));
        reply->Src = Id;
        reply->Dst = message->Src;
        reply->Term = State->CurrentTerm;
        reply->VoteGranted = false;
        return std::make_unique<TResult>(TResult {
            .Message = reply
        });
    } else if (message->Term == State->CurrentTerm) {
        bool accept = false;
        if (State->VotedFor == 0) {
            accept = true;
        } else if (State->VotedFor == message->CandidateId && message->LastLogTerm > State->LogTerm()) {
            accept = true;
        } else if (State->VotedFor == message->CandidateId && message->LastLogTerm == State->LogTerm() && message->LastLogIndex >= State->Log.size()) {
            accept = true;
        }

        auto reply = NewHoldedMessage<TRequestVoteResponse>(static_cast<uint32_t>(EMessageType::REQUEST_VOTE_RESPONSE), sizeof(TRequestVoteResponse));
        reply->Src = Id;
        reply->Dst = message->Src;
        reply->Term = message->Term;
        reply->VoteGranted = accept;

        return std::make_unique<TResult>(TResult {
            .NextState = std::make_unique<TState>(TState{
                .CurrentTerm = message->Term,
                .VotedFor = message->CandidateId
            }),
            .Message = reply,
        });
    }

    return nullptr;
}

std::unique_ptr<TResult> TRaft::OnRequestVote(TMessageHolder<TRequestVoteResponse> message) {
    if (message->Term > State->CurrentTerm) {
        return std::make_unique<TResult>(TResult {
            .NextState = std::make_unique<TState>(TState{
                .CurrentTerm = State->CurrentTerm,
                .VotedFor = State->VotedFor,
            }),
            .NextStateName = EState::FOLLOWER,
            .UpdateLastTime = true
        });
    }
    auto votes = VolatileState->Votes;
    if (message->VoteGranted && message->Term == State->CurrentTerm) {
        votes.insert(message->Src);
    }

    int nvotes = votes.size()+1;
    std::cout << "Need/total: " << MinVotes << "/" << nvotes << "\n";
    if (nvotes >= MinVotes) {
        auto value = State->Log.size()+1;
        decltype(VolatileState->NextIndex) nextIndex;
        for (auto [id, _] : Nodes) {
            nextIndex.emplace(id, value);
        }
        return std::make_unique<TResult>(TResult {
            .NextState = std::make_unique<TState>(TState{
                .CurrentTerm = State->CurrentTerm,
                .VotedFor = State->VotedFor,
            }),
            .NextVolatileState = std::make_unique<TVolatileState>(TVolatileState {
                .CommitIndex = VolatileState->CommitIndex,
                .LastApplied = VolatileState->LastApplied,
                .NextIndex = nextIndex,
            }),
            .NextStateName = EState::LEADER,
            .UpdateLastTime = true,
        });
    }

    auto nextVolatileState = *VolatileState;
    nextVolatileState.SetVotes(votes);
    return std::make_unique<TResult>(TResult {
        .NextState = std::make_unique<TState>(TState{
            .CurrentTerm = State->CurrentTerm,
            .VotedFor = State->VotedFor,
        }),
        .NextVolatileState = std::make_unique<TVolatileState>(
            nextVolatileState
        ),
    });
}

std::unique_ptr<TResult> TRaft::OnAppendEntries(TMessageHolder<TAppendEntriesRequest> message) {
    if (message->Term < State->CurrentTerm) {
        auto reply = NewHoldedMessage<TAppendEntriesResponse>(static_cast<uint32_t>(EMessageType::APPEND_ENTRIES_RESPONSE), sizeof(TAppendEntriesResponse));
        reply->Src = Id;
        reply->Dst = message->Src;
        reply->Term = State->CurrentTerm;
        reply->Success = false;
        reply->MatchIndex = 0;
        return std::make_unique<TResult>(TResult {
            .UpdateLastTime = true,
            .Message = reply,
        });
    }

    assert(message->Term == State->CurrentTerm);

    uint64_t matchIndex = 0;
    uint64_t commitIndex = VolatileState->CommitIndex;
    bool success = false;
    if (message->PrevLogIndex == 0 ||
        (message->PrevLogIndex <= State->Log.size()
            && State->LogTerm(message->PrevLogIndex) == message->PrevLogIndex))
    {
        success = true;
        auto index = message->PrevLogIndex;
        auto log = State->Log;
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

    auto reply = NewHoldedMessage<TAppendEntriesResponse>(static_cast<uint32_t>(EMessageType::APPEND_ENTRIES_RESPONSE), sizeof(TAppendEntriesResponse));
    reply->Src = Id;
    reply->Dst = message->Src;
    reply->Term = State->CurrentTerm;
    reply->Success = false;
    reply->MatchIndex = matchIndex;

    auto nextVolatileState = *VolatileState;
    nextVolatileState.SetCommitIndex(commitIndex);

    return std::make_unique<TResult>(TResult {
        .NextVolatileState = std::make_unique<TVolatileState>(nextVolatileState),
        .NextStateName = EState::FOLLOWER,
        .UpdateLastTime = true,
        .Message = reply,
    });
}

std::unique_ptr<TResult> TRaft::OnAppendEntries(TMessageHolder<TAppendEntriesResponse> message) {
    if (message->Term != State->CurrentTerm) {
        return nullptr;
    }

    auto nodeId = message->Src;
    if (message->Success) {
        auto matchIndex = std::max(VolatileState->MatchIndex[nodeId], message->MatchIndex);
        auto nextVolatileState = *VolatileState;
        nextVolatileState
            .MergeMatchIndex({{nodeId, matchIndex}})
            .MergeNextIndex({{nodeId, message->MatchIndex+1}})
            .CommitAdvance(Nservers, State->Log.size(), *State);
        return std::make_unique<TResult>(TResult {
            .NextVolatileState = std::make_unique<TVolatileState>(nextVolatileState)
        });
    } else {
        auto nextVolatileState = *VolatileState;
        nextVolatileState.MergeNextIndex({{nodeId, std::max((uint64_t)1, VolatileState->NextIndex[nodeId]-1)}});
        return std::make_unique<TResult>(TResult {
            .NextVolatileState = std::make_unique<TVolatileState>(nextVolatileState)
        });
    }
}

TMessageHolder<TRequestVoteRequest> TRaft::CreateVote() {
    auto mes = NewHoldedMessage<TRequestVoteRequest>(
        static_cast<uint32_t>(EMessageType::REQUEST_VOTE_REQUEST),
        sizeof(TRequestVoteRequest));
    mes->Src = Id;
    mes->Dst = 0;
    mes->Term = State->CurrentTerm+1;
    mes->CandidateId = Id;
    mes->LastLogIndex = State->Log.size();
    mes->LastLogTerm = State->Log.empty()
        ? 0
        : State->Log.back()->Term;
    return mes;
}

std::vector<TMessageHolder<TAppendEntriesRequest>> TRaft::CreateAppendEntries() {
    std::vector<TMessageHolder<TAppendEntriesRequest>> res;
    for (auto [nodeId, _] : Nodes) {
        auto prevIndex = VolatileState->NextIndex[nodeId] - 1;
        auto lastIndex = std::min(prevIndex+1, (uint64_t)State->Log.size());
        if (VolatileState->MatchIndex[nodeId]+1 < VolatileState->NextIndex[nodeId]) {
            lastIndex = prevIndex;
        }

        auto mes = NewHoldedMessage<TAppendEntriesRequest>(
            static_cast<uint32_t>(EMessageType::APPEND_ENTRIES_REQUEST), sizeof(TAppendEntriesRequest));

        mes->Src = Id;
        mes->Dst = nodeId;
        mes->Term = State->CurrentTerm;
        mes->LeaderId = Id;
        mes->PrevLogIndex = prevIndex;
        mes->PrevLogTerm = State->LogTerm(prevIndex);
        mes->LeaderCommit = std::min(VolatileState->CommitIndex, lastIndex);
        mes->Nentries = lastIndex - prevIndex;
        std::vector<TMessageHolder<TMessage>> payload;
        payload.reserve(lastIndex - prevIndex);
        for (auto i = prevIndex; i < lastIndex; i++) {
            payload.push_back(State->Log[i]);
        }
        mes.Payload = std::move(payload);
    }
    return res;
}

std::unique_ptr<TResult> TRaft::Follower(ITimeSource::Time now, TMessageHolder<TMessage> message) {
    if (auto maybeTimeout = message.Maybe<TTimeout>()) {
        if (now - LastTime > TTimeout::Election) {
            return std::make_unique<TResult>(TResult {
                .NextStateName = EState::CANDIDATE,
                .UpdateLastTime = true
            });
        }
    } else if (auto maybeRequestVote = message.Maybe<TRequestVoteRequest>()) {
        return OnRequestVote(std::move(maybeRequestVote.Cast()));
    } else if (auto maybeAppendEntries = message.Maybe<TAppendEntriesRequest>()) {
        return OnAppendEntries(maybeAppendEntries.Cast());
    }
    return nullptr;
}

std::unique_ptr<TResult> TRaft::Candidate(ITimeSource::Time now, TMessageHolder<TMessage> message) {
    if (auto maybeTimeout = message.Maybe<TTimeout>()) {
        if (now - LastTime > TTimeout::Election) {
            return std::make_unique<TResult>(TResult {
                .NextState = std::make_unique<TState>(TState {
                    .CurrentTerm = State->CurrentTerm+1,
                    .VotedFor = Id,
                }),
                .NextVolatileState = std::make_unique<TVolatileState>(),
                .UpdateLastTime = true,
                .Message = CreateVote()
            });
        }
    } else if (auto maybeResponseVote = message.Maybe<TRequestVoteResponse>()) {
        return OnRequestVote(std::move(maybeResponseVote.Cast()));
    } else if (auto maybeRequestVote = message.Maybe<TRequestVoteRequest>()) {
        return OnRequestVote(std::move(maybeRequestVote.Cast()));
    } else if (auto maybeAppendEntries = message.Maybe<TAppendEntriesRequest>()) {
        return OnAppendEntries(maybeAppendEntries.Cast());
    }
    return nullptr;
}

std::unique_ptr<TResult> TRaft::Leader(ITimeSource::Time now, TMessageHolder<TMessage> message) {
    if (auto maybeTimeout = message.Maybe<TTimeout>()) {
        if (now - LastTime > TTimeout::Heartbeat) {
            return std::make_unique<TResult>(TResult {
                .UpdateLastTime = true,
                .Messages = CreateAppendEntries()
            });
        }
    } else if (auto maybeAppendEntries = message.Maybe<TAppendEntriesResponse>()) {
        return OnAppendEntries(maybeAppendEntries.Cast());
    } else if (auto maybeCommandRequest = message.Maybe<TCommandRequest>()) {
        auto command = maybeCommandRequest.Cast();
        auto log = State->Log;
        auto dataSize = command->Len - sizeof(TCommandRequest);
        auto entry = NewHoldedMessage<TLogEntry>(static_cast<uint32_t>(EMessageType::LOG_ENTRY), sizeof(TLogEntry)+dataSize);
        memcpy(entry->Data, command->Data, dataSize);
        log.push_back(entry);
        auto nextState = std::make_unique<TState>(TState {
            .CurrentTerm = State->CurrentTerm,
            .VotedFor = State->VotedFor,
            .Log = std::move(log),
        });
        auto nextVolatileState = *VolatileState;
        nextVolatileState.CommitAdvance(Nservers, log.size(),  *State);
        return std::make_unique<TResult>(TResult {
            .NextState = std::move(nextState),
            .NextVolatileState = std::make_unique<TVolatileState>(nextVolatileState),
            .Message = NewHoldedMessage<TCommandResponse>(static_cast<uint32_t>(EMessageType::COMMAND_RESPONSE), sizeof(TCommandResponse)),
        });
    } else if (auto maybeVoteRequest = message.Maybe<TRequestVoteRequest>()) {
        return OnRequestVote(std::move(maybeVoteRequest.Cast()));
    } else if (auto maybeVoteResponse = message.Maybe<TRequestVoteResponse>()) {
        // skip additional votes
    }
    return nullptr;
}

void TRaft::Become(EState newStateName) {
    if (StateName != newStateName) {
        StateName = newStateName;
        Process(NewTimeout());
    }
}

void TRaft::Process(TMessageHolder<TMessage> message, INode* replyTo) {
    auto now = TimeSource->Now();

    if (message.IsEx()) {
        auto messageEx = message.Cast<TMessageEx>();
        State->CurrentTerm = messageEx->Term;
        State->VotedFor = 0;
        StateName = EState::FOLLOWER;
    }
    std::unique_ptr<TResult> result;
    switch (StateName) {
    case EState::FOLLOWER:
        result = Follower(now, std::move(message));
        break;
    case EState::CANDIDATE:
        result = Candidate(now, std::move(message));
        break;
    case EState::LEADER:
        result = Leader(now, std::move(message));
        break;
    default:
        throw std::logic_error("Unknown state");
    }

    ApplyResult(now, std::move(result), replyTo);
}

void TRaft::ApplyResult(ITimeSource::Time now, std::unique_ptr<TResult> result, INode* replyTo) {
    if (!result) {
        return;
    }
    if (result->UpdateLastTime) {
        LastTime = now;
    }
    if (result->NextState) {
        State = std::move(result->NextState);
    }
    if (result->NextVolatileState) {
        VolatileState = std::move(result->NextVolatileState);
    }
    if (result->Message) {
        if (result->Message.Maybe<TCommandResponse>()) {
            if (replyTo) {
                replyTo->Send(result->Message);
            }
        } else {
            auto messageEx = result->Message.Cast<TMessageEx>();
            if (messageEx->Dst == 0) {
                for (auto& [id, v] : Nodes) {
                    v->Send(messageEx);
                }
            } else {
                Nodes[messageEx->Dst]->Send(messageEx);
            }
        }
    }
    if (!result->Messages.empty()) {
        for (auto& m : result->Messages) {
            Nodes[m->Dst]->Send(m);
        }
    }
    if (result->NextStateName != EState::NONE) {
        StateName = result->NextStateName;
    }
}
