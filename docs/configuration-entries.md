# Live joint membership

The runtime accepts membership changes through a joint-consensus log sequence.
`TRaft::ProposeJointConfiguration(newVoters)` appends the joint entry, which
activates both the old and new voter majorities. After the joint entry commits,
`TRaft::FinalizeConfiguration()` appends its stable successor. The stable set
becomes active only after that successor commits. Elections, commit advancement
and strong-read confirmation use the active stable or joint quorum.

Both APIs require storage that persists the commit index and original voter
binding. `TDurableState` provides those guarantees. In-memory `TState` and legacy
stores cannot start a live transition. The leader must have a committed entry in
its current term. Every newly added voter must first be staged as a learner and
have a correlated replication acknowledgement through the leader's committed
boundary. The leader must be a member of the new set. The currently supported
API cannot remove its active leader; this library does not yet provide a
leadership-transfer API. Every node must have replication endpoints for all
voters before it accepts the transition. Endpoint discovery and configuration
distribution remain responsibilities of the embedding application.
On restart, construct each Raft object with the persisted original voter set and
configure later members as learners in that bootstrap view; the retained log
then recovers their active roles. Supplying the latest voter set as if it were
the original binding is rejected.

These are `TRaft` APIs. `TRaftServer` currently starts outbound client-response
readers from the roles present when `Serve()` starts and does not reconcile that
reader set after a live promotion. Raft replication and quorum processing are
updated live, but a promoted follower that forwards client requests may need a
server restart before its forwarded responses are handled. Hot server role
reconciliation remains a follow-up.

Incoming AppendEntries configuration records are decoded and the complete
candidate suffix is scanned before a higher RPC term is persisted. The scan
checks predecessor identity, transition order, committed-prefix constraints,
voter binding and endpoint availability. Same-term configuration entries must
match byte for byte. Replacing an uncommitted configuration suffix recomputes
the active roles and quorum from the surviving log; committed entries cannot be
truncated. A malformed or semantically invalid candidate suffix is rejected
without changing the term, commit index, log or response state.

The record payload remains MRFTC001: magic[8], phase[4] (joint=1, stable=2),
entry term[8], previous configuration index[8], previous configuration
term[8], old count[4], new count[4], old voter IDs[4 each], new voter IDs[4
each], and CRC32[4] over the preceding payload. The outer entry term must match
the encoded term and its flags must equal `EConfiguration`. CRC checks accidental
corruption; it is not authentication. All voters must run a compatible binary
before a transition because older versions reject this entry type.

Membership-aware compaction and joint snapshot installation are still open.
Compaction remains blocked after retained membership history, and a lagging
peer that requires snapshot transfer across a transition is not yet supported.
Existing fixed-membership snapshots and legacy stores do not prove the active
voter set after a transition. Do not remove or edit committed state to bypass
these limits.

Deterministic durable-store tests cover learner catch-up, voter addition and
nonleader removal, per-node activation, joint commit propagation, stable
finalization, rejection of a bad predecessor before term mutation, and rollback
of an uncommitted joint suffix. The test suite also checks recovery of retained
joint and stable histories, including post-restart role recovery after adding
and removing a voter. This is a first live transition slice; it does not
claim snapshot continuity across membership changes or explicit leadership
transfer.
