# Durable configuration snapshot metadata: v0.1.0-alpha.45

Bound `TState` and `TDurableState` instances can install and recover MRFTS002
joint and MRFTS003 stable snapshots so offline tools can validate configuration
history after restart. A store must already have a voter binding, and
`snapshot.Voters` must equal that binding. Durable recovery checks the same
invariant before exposing the image. An unbound store cannot infer its original
voter set from configuration metadata and continues to reject the snapshot.

The durable file layout is unchanged. The backend persists the snapshot together
with its existing voter binding; it does not reinterpret that binding from the
metadata. The history scanner also verifies that a joint baseline's old voters
match the bound set.

In alpha.45 these images were persistence for offline validation only. The
alpha.46 follow-up enables runtime recovery and transfer of MRFTS003 stable
snapshots when the voter list equals the node's existing fixed membership.
Compaction carries forward that stable configuration identity. MRFTS002 joint
snapshots and configuration log entries remain disabled in the live runtime.
Stable snapshots that claim another voter set are still rejected because
membership transition semantics are not implemented.

Tests cover in-memory installation, durable joint and stable round-trips, bound
voter mismatch rejection without rewriting storage, offline scan baseline checks,
stable runtime restore/transfer/compaction, and continued joint snapshot rejection.
No independent reviewer approval is claimed.
