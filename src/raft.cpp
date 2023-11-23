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

std::unique_ptr<TResult> TRaft::Follower(ITimeSource::Time now, TMessageHolder<TMessage> message) {
    return nullptr;
}

std::unique_ptr<TResult> TRaft::Candidate(ITimeSource::Time now, TMessageHolder<TMessage> message) {
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
    if (result->NextStateName) {
        StateName = static_cast<EState>(result->NextStateName);
    }
}
