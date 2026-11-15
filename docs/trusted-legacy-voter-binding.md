# Trusted legacy voter binding: v0.1.0-alpha.50

Older durable MRFTD001/MRFTD002 files do not persist the complete fixed voter
set. Raft cannot infer that set safely from a vote, a legacy snapshot voter
list, or the current process's peer list alone.

`TDurableState::BindVotersFromTrustedBaseline(voters)` provides an explicit
migration path for an unbound, recovered durable store. The caller must supply
the complete original fixed voter set from an independently trusted source,
such as the cluster's verified deployment record. The operation checks that the
stored vote belongs to the set, any legacy snapshot names the same voters and
has no configuration metadata, retained terms are valid, and no reserved
configuration entries are present. It then atomically republishes the durable
state with its voter binding before the store is passed to `TRaft`.

This API records the caller's assertion; it cannot prove the assertion is
correct, discover prior membership transitions hidden by a legacy snapshot, or
authorize a membership change. Existing `BindVoters` behavior is unchanged.
Do not call the migration API when the original complete voter set is unknown.
The migration applies to `TDurableState`; legacy `TDiskState` remains unable to
persist voter bindings. Since alpha.52, Raft rejects recovered log-only durable
stores with no binding, so this migration must be completed before startup.

Tests cover MRFTD001, MRFTD002 and MRFTD003 recovery, rejection
of inconsistent vote/snapshot baselines and configuration log entries without
rewriting the source file, and the current MRFTD004 binding after publication
and restart. Legacy formats recover their commit floor at the snapshot boundary.

Self-review completed. No independent reviewer approval is claimed.
