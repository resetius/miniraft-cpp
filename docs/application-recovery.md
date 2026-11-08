# Recovered application progress: v0.1.0-alpha.13

After restarting a durable application, Raft's volatile commit index began at
zero. ProcessCommitted unconditionally copied that zero into LastAppliedIndex,
discarding the application's recovered marker. Re-establishing commitment then
reapplied old commands. A file-backed SQL/TDiskState regression reproduced an
INSERT being applied twice after a clean restart. A separate restart scenario
also exposed the loss of committed-prefix protection before leader contact.

TRequestProcessor construction now initializes a commit floor from the recovered
application marker. The marker must not exceed the recovered local log or the
signed log-index range; invalid pairings throw before changing the commit index.
The floor preserves any higher commitment already known to Raft. The existing
AppendEntries validation then protects this applied prefix immediately, while
allowing replacement of an uncommitted suffix. The restoration helper is private
to Raft and the request processor.

ProcessCommitted returns when commitment is at or below application progress.
It never rewinds the marker or computes LastAppliedIndex+1 in that case. New
commitment applies only the remaining entries. Framework progress still advances
over internal no-op entries; an application need only persist progress alongside
its own mutations, so harmless no-ops can be revisited after restart.

Recovery contract for embedders:

* Restore application state and LastAppliedIndex atomically before constructing
  TRequestProcessor, and construct the processor before accepting network traffic.
* A nonzero marker must represent genuinely committed progress in the same local
  Raft log. Do not derive it from the log's last index: a suffix may be uncommitted.
* A volatile application starts at zero and rebuilds after commitment is known.
* Keep application and Raft storage paired. An application ahead of the local log
  now fails startup instead of silently forgetting or skipping progress.

An applied marker is trusted evidence of commitment; a range check cannot detect
a forged marker, mismatched data at the same index or application state persisted
without its marker. Snapshot/compaction recovery is not supported. Strong reads
still require a current-term commit and a fresh majority; the recovered floor
does not replace those checks. This follows the monotonic application-progress
and state-machine safety rules in [Raft's Figure 2 and Figure 3](https://raft.github.io/raft.pdf).

Six new tests failed against the prior implementation and now pass:

1. Restarted application progress survives an early timer and is not replayed.
2. Lower commitment input cannot rewind application progress.
3. A recovered applied prefix resists conflicting RPCs while its suffix can change.
4. Application progress ahead of the recovered log is rejected without mutation.
5. A three-node restart preserves durable application histories, rebuilds a
   volatile application, replaces an uncommitted old-leader suffix and tolerates
   delayed, reversed and duplicate traffic after a new election. It also checks
   that recovered commitment cannot authorize new-term reads by itself.
6. Real SQL and legacy Raft files reopen without repeating a committed INSERT.

All 175 tests across 11 CTest targets pass locally under ASan/UBSan with SQLite;
the Release core suite also passes. Self-review covered initialization ordering,
range checks before mutation, monotonicity, no-op handling, conflict protection,
current-term read gating and repeat application calls. No independent reviewer
approval is claimed.

The simulator retains stable term/vote/log objects and reconstructs Raft volatile
state; it is a deterministic process-restart model, not torn-write simulation.
The SQL integration test performs clean file close/reopen. These additions do not
establish power-loss durability for TDiskState or SQLite, authenticated pairing
of stores, exhaustive fault schedules, snapshots or retry deduplication. Those
and independent review remain production gates.
