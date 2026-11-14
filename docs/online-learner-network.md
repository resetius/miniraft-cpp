# Network server learner staging: v0.1.0-alpha.40

`TRaftServer<TSocket>` now exposes `AddLearnerTarget(id, endpoint)` and
`RemoveLearnerTarget(id)`, using a `shared_ptr<TNode<TSocket>>` endpoint. Adding
a learner registers it with Raft and the server's drain set, so the normal
server loop can send its replication and snapshot traffic without restarting.
Removing it drops its Raft progress and removes it from future server drains;
the method returns the endpoint so the embedding application can manage its
transport lifetime. The voter list and quorum calculations remain unchanged.

Construct the remote node with the right address and socket factory, and make
sure its local Raft instance is configured as a learner with the same voter set.
The storage on the current leader must have a bound voter list. The learner
still must have a configured return path to the voters. The server does not
discover, authenticate or configure the remote process.

These calls must run on the same event-loop thread as `TRaftServer` and be
serialized with Raft processing. They may be called before `Serve()` or from a
serialized callback after serving begins. Calls from other threads and
reentrant calls from transport or application callbacks are unsupported.
After removal, a write already in progress or data already queued in the
endpoint may remain. Keep the returned endpoint alive while that I/O can still
use it, then shut it down according to the embedding application's transport
lifecycle.

The KV CLI still discovers peers at startup. Staging is leader-local and is
lost on restart or failover; the new leader and the application must reconcile
it. This adds network transport support for non-voting catch-up only. Voter
promotion/removal, replicated joint consensus and snapshot installation across
membership transitions remain unsupported.

The deterministic server test checks target registration, voter/quorum
invariance, removal and rollback after a nonleader rejection. The existing
learner tests cover snapshot catch-up and stale responses after removal. No
independent reviewer approved this milestone.
