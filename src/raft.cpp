#include <memory>
#include <stdexcept>

#include "raft.h"
#include "messages.h"

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
    return nullptr;
}

std::unique_ptr<TResult> TRaft::OnAppendEntries(TMessageHolder<TAppendEntriesRequest> message) {
    return nullptr;
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
        : State->Log.back().Term;
    return mes;
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
                    .Log = State->Log
                }),
                .NextVolatileState = std::make_unique<TVolatileState>(),
                .UpdateLastTime = true,
                .Message = CreateVote()
            });
        }
    } else if (auto maybeResponseVote = message.Maybe<TRequestVoteResponse>()) {
        // TODO
    } else if (auto maybeRequestVote = message.Maybe<TRequestVoteRequest>()) {
        return OnRequestVote(std::move(maybeRequestVote.Cast()));
    } else if (auto maybeAppendEntries = message.Maybe<TAppendEntriesRequest>()) {
        return OnAppendEntries(maybeAppendEntries.Cast());
    }
    return nullptr;
}

std::unique_ptr<TResult> TRaft::Leader(ITimeSource::Time now, TMessageHolder<TMessage> message) {
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
