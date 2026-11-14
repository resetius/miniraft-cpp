# Trusted voter migration startup boundary: v0.1.0-alpha.51

The alpha.50 `TDurableState::BindVotersFromTrustedBaseline` migration must run
before a `TRaft` instance uses a recovered legacy store. A nonfresh legacy store
without a persisted binding can still start under its configured fixed peers for
compatibility. If migration were allowed afterward, the durable state could be
bound to a different voter set while the live Raft object continued using the
peer set it received at construction.

`IState::MarkRaftStarted` lets the runtime notify storage after all startup
validation succeeds. `TDurableState` records that attachment in memory and
rejects trusted legacy migration from then on, before modifying or publishing
the durable file. The marker is process-local: after restart, recovery creates a
new state object and the operator can migrate it before constructing Raft.

The guard does not infer or prove the original voter set, and it does not add
live voter reconfiguration. Tests verify that a post-start migration to a
different set is rejected without changing the binding or file; existing tests
continue to cover migration before startup and recovered restart behavior.

The [alpha.52 follow-up](legacy-voter-proof-startup.md) closes the remaining
startup gap: recovered log-only durable stores without a voter binding now fail
before Raft starts, requiring this migration path or another valid voter proof.

Self-review completed; no independent reviewer approval is claimed.
