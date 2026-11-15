# Online learner replication staging

`TRaft::AddLearnerTarget(id, endpoint)` lets the current leader add a non-voting
replication target without restarting that Raft object. `RemoveLearnerTarget(id)`
removes a local target and returns its endpoint. The voter set and persisted
binding do not change. Staged learners receive the ordinary log or a snapshot
when needed, but never count toward election, commit, or strong-read quorums.
`IsLearnerCaughtUp` remains a point-in-time log watermark, not an authorization
to promote or a guarantee of application health.
`IsLearnerCommitAcknowledged` separately reports whether a correlated successful
replication reply confirms the leader's current Raft commit boundary while the
learner also matches the leader's log. It does not report application-applied
progress and does not authorize promotion.

These are serialized embedding APIs, not a replicated cluster configuration or a
promotion API. The caller must ensure the destination is configured as a learner
with the same voter set, arrange and authenticate its transport, and keep its
endpoint alive while asynchronous operations may still use it. After removal,
use the returned endpoint to coordinate safe transport shutdown; queued traffic
can still exist. The caller must serialize add/remove with Raft processing and
must not re-enter them from transport callbacks. Duplicate IDs, voters, the local
ID, null endpoints and calls on nonleaders are rejected. Staging requires storage
with an already-bound matching voter set; unbound and legacy stores cannot stage.

Registration is local to this Raft object and is not persisted or propagated to
other voters. The embedding application must reconcile targets on a new leader
and after restart; a previous leader retains its local list if it becomes a
follower. Removing a target drops replication progress and pending RPCs, and a
subsequent add starts a fresh probe. Delayed responses cannot satisfy new RPCs.
The KV CLI still uses static transport setup. `TRaftServer` runtime integration
is described in [network learner staging](online-learner-network.md). Older
binaries must not be mixed with learner configurations because they treat
unmarked peers as voters.

Deterministic tests cover live add, snapshot catch-up, voter partitions and strong
reads, removal and re-addition with delayed append replies, removal during a
multi-chunk snapshot with delayed acknowledgements, invalid calls, and refusal
on unbound or legacy stores. Durable-store tests verify that staging does not
rewrite the binding and is absent on restart. A leader-change test verifies
that an embedding application must re-register the target on the new leader.
Dynamic voter promotion/removal, replicated joint consensus, persistent discovery,
and membership-aware snapshot installation are still required before live voting
membership changes are supported.
