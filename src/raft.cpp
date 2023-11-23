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
    , LastTime(TimeSource->now())
{ }

std::unique_ptr<TResult> TRaft::Follower(uint64_t now, const TMessageHolder<TMessage>& message) {
    return nullptr;
}

void TRaft::ApplyResult(uint64_t now, std::unique_ptr<TResult> result, INode* replyTo) {
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
