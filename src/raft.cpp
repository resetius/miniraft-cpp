#include "raft.h"
#include "messages.h"

TRaft::TRaft(int node, const std::vector<TNode>& nodes, const TTimeSource& ts)
    : Id(node)
    , Nodes(nodes)
    , TimeSource(ts)
    , MinVotes((nodes.size()+1/2))
    , Npeers(nodes.size())
    , Nservers(nodes.size()+1)
    , StateFunc([&](uint64_t now, const TMessageHolder<TMessage> &message) {
        return follower(now, message);
    })
    , LastTime(TimeSource.now())
{ }

std::unique_ptr<TResult> TRaft::follower(uint64_t now, const TMessageHolder<TMessage>& message) {
    return nullptr;
}

void TRaft::applyResult(uint64_t now, std::unique_ptr<TResult> result, TNode* replyTo) {
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
                replyTo->send(result->Message);
            }
        } else {
            auto messageEx = result->Message.Cast<TMessageEx>();
            if (messageEx->Dst == 0) {
                for (auto& v : Nodes) {
                    v.send(messageEx);
                }
            } else {
                Nodes[messageEx->Dst].send(messageEx);
            }
        }
    }
    if (!result->Messages.empty()) {
        for (auto& m : result->Messages) {
            Nodes[m->Dst].send(m);
        }
    }
    if (result->NextStateFunc) {
        StateFunc = result->NextStateFunc;
    }
}
