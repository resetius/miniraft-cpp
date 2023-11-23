#include "raft.h"

TRaft::TRaft(int node, const std::vector<TNode>& nodes, const TTimeSource& ts)
    : Id(node)
    , Nodes(nodes)
    , TimeSource(ts)
    , MinVotes((nodes.size()+1/2))
    , Npeers(nodes.size())
    , Nservers(nodes.size()+1)
    , StateFunc([&](uint64_t now, const TMessageHolder<TMessage> &message) {
        follower(now, message);
    })
    , LastTime(TimeSource.now())
{ }
