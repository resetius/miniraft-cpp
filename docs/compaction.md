# Compaction and InstallSnapshot: v0.1.0-alpha.16

The [learner follow-up](learners.md) adds non-voting replication and durable
voter binding. Current MRFTD004 images also persist commit progress; the reader
continues to accept the earlier v1/v2/v3 images below.

This milestone adds prefix deletion and follower state transfer to the alpha.15
KV checkpoint foundation. TState and Linux TDurableState support compacted logs;
legacy TDiskState does not. KV implements snapshot validation/restoration. SQL
support is described in [the SQL snapshot follow-up](sql-snapshots.md); the dummy
state machine remains unsupported for compaction and installation.

## Operation and compatibility

The existing Linux KV option `--durable --snapshots N` now compacts after N more
applied indices. Raft::Compact captures committed applied state, validates its
application image and atomically stores the snapshot plus retained suffix. The
separate `state.snapshot.<id>` remains a checkpoint export for compatibility; the
embedded snapshot in `state.durable.<id>` is authoritative after compaction.

Stop and upgrade every voter before enabling this release in an existing cluster.
Old binaries cannot read compacted durable images or process InstallSnapshot RPCs.
The meaning of `--snapshots` changes from checkpoint-only to actual compaction.
Mixed-version operation is unsupported. Existing v1 durable files and alpha.15
checkpoints can be read without destructive migration. The first compaction or
remote installation writes the current durable format; downgrading that store is unsupported. Fixed voter
IDs must match the embedded snapshot. Static `--node` edits remain unsafe as a
membership-change mechanism.

Startup restores an application behind the embedded snapshot before request
processing, including after a crash between durable publication and application
replacement. A newer standalone
checkpoint can restore progress within the retained suffix; an older one never
replaces embedded state. With periodic snapshots enabled, an existing corrupt
standalone file is still rejected conservatively. Snapshot recovery never resets
the persisted current term or vote.

## Boundaries and atomic persistence

Raft startup validates a recovered snapshot before binding voters: its image,
index and term must agree with the storage boundary, its voter set must match
the fixed configuration, and the last term must be consistent with the
boundary and current term. It scans retained terms for nonzero, nondecreasing
values no newer than the current term and verifies the recorded last term. A
nonzero boundary without an image is rejected. Custom backends must report
accurate entry metadata, validate payloads, and supply a consistent, serialized
view; these checks are not a complete storage audit or proof of snapshot contents.
The scan uses `RetainedLogTerm`; backends relying on its default implementation
may read full entry payloads at startup.

SnapshotIndex/SnapshotTerm identify the last included entry. Get only returns
entries after SnapshotIndex, while LogTerm at the boundary returns SnapshotTerm.
LastLogIndex and all RPC/application indices remain absolute. Appends, truncation,
votes, current-term commitment and strong reads use these absolute boundaries.
Truncation cannot remove a snapshot. Election no-op creation also checks signed
index exhaustion, now reachable without allocating an enormous prefix. Memory
and durable storage reject a direct append once the last index is INT64_MAX,
before mutating the log or publishing an image. Appending the entry at INT64_MAX
itself remains valid when the previous index is INT64_MAX - 1.

An installation retains the suffix only when the local entry at the included
index has the same term. Otherwise its uncommitted log is discarded. A follower
does not install an image at or below known commitment, so stale snapshots cannot
rewind application state. Retained suffix entries are applied only after a leader
establishes their commitment.

MRFTD004 publishes hard state, the persisted commit boundary, voter binding,
complete encoded snapshot and suffix in one atomic image, using the existing
temp-write/file-fsync/rename/directory-fsync protocol. Its header remains
magic[8], payload length[8], CRC[4]. The payload is CurrentTerm[8], VotedFor[4],
retained count[8], CommitIndex[8], voter count[4], voter IDs, snapshot length[8],
the encoded snapshot, then term/flags/length/data records. The CRC covers the v4
magic plus payload. Recovery checks `SnapshotIndex <= CommitIndex <= LastLogIndex`,
bounds the embedded image and retained count, and validates nondecreasing terms
beginning at SnapshotTerm. Older v1/v2/v3 images remain readable and recover
their commit floor as SnapshotIndex; a successful later publication writes v4.
Malformed recovery does not rewrite the image. Live publication failure is fatal.

Followers publish a newly learned commit index before acknowledging
AppendEntries. Leaders publish quorum commit advancement before application
processing. Snapshot installation commits at least the included boundary, and
storage refuses to truncate a committed entry. The legacy TDiskState backend
does not persist CommitIndex.

## Transfer protocol

The previously reserved message types 6/7 now carry InstallSnapshot requests and
responses. A request identifies leader/term, boundary index/term, total image size,
offset and final-chunk flag. Responses carry the boundary, next offset, success
and an explicit Installed flag. Seqno correlates acknowledgements to the active
per-peer RPC. Frames are capped at a 64 KiB body before allocation; complete
images remain bounded at 64 MiB with at most 4,096 fixed voters.

Leaders use stop-and-wait chunks when NextIndex is at or below their compacted
boundary. Offset and RPC context survive retries within the leadership term;
delayed/duplicate responses cannot confirm another image. Followers hold one
bounded partial image, validate duplicate bytes, reject gaps and bind assembly
to leader, term, index, boundary term and total length. A restarted follower asks
for offset zero; partial state is not persisted. Transfers restart on newer terms
or newer images, and valid chunks refresh the election timer.

Partial chunks never change application progress or MatchIndex. Completion first
checks CRC, metadata, fixed voters and the application payload without changing
live application state. The follower durably installs snapshot plus suffix, then
replaces application state, then acknowledges Installed. Failure after durable
installation terminates the process; startup restores the selected image. A lost
completion ACK can be retried without applying the image again.

Only Installed completion advances leader MatchIndex. Neither partial nor final
snapshot acknowledgements count as fresh-read confirmations: subsequent
AppendEntries must establish read barriers and replicate any suffix. A compacted
follower can return its boundary in a failed AppendEntries MatchIndex field as a
probe hint. That hint advances only NextIndex; a matching AppendEntries probe is
required before counting the index as replicated or acknowledging reads.

This implements the boundary/suffix rules of [Raft section 7 and Figure 13](https://raft.github.io/raft.pdf).
Transfer is deliberately serialized and bounded; it is not disk-streaming or a
parallel multi-image protocol.

## Validation and review

Fourteen new deterministic core/storage tests cover lagging follower catch-up and
tail replication; restart mid-transfer; delayed, duplicate and impossible ACKs;
leadership changes; strong-read gating; compacted-prefix probing; corrupt images
and invalid headers; voter and application mismatch; retained but uncommitted
suffixes; memory/durable boundary behavior; index exhaustion; v2 semantic
corruption; disk-space reclamation; and first/replacement image selection at all
five publication boundaries for retained/discarded suffixes.

One new transport test checks fragmented request/response round trips and rejects
empty/oversized chunks before body allocation. The existing CLI smoke now checks
v2 publication and restoration with periodic snapshotting disabled. A new real
three-process test compacts a two-voter majority, restarts both to clear queued
old log traffic, starts an empty third voter, verifies local reads after snapshot
installation, replicates a tail, then restarts that voter and checks recovery.
It uses bounded waits, separately from deterministic virtual-time fault schedules.

That integration test exposed a pre-existing election tie: all nodes used the
same RNG seed and campaigned in lockstep. Node-specific nonzero deterministic
seeds now provide distinct election schedules; a simultaneous-startup regression
verifies election progress after the initial split vote. This is scheduling
diversification, not a security/entropy source.

Local validation: 207 unit tests and two CLI tests pass under ASan/UBSan across
15 CTest targets; Release core checks pass. Self-review covered boundary arithmetic,
suffix selection, persistence/application/ACK ordering, parser allocation limits,
transfer context and retries, stale snapshots, fresh-read separation, recovery
ordering and backward v1 decoding. No independent reviewer approval is claimed.
Publication-stage process crashes do not replace hardware power-cut testing.

## Remaining work

Membership remains fixed. Learners, catch-up gates, replicated joint-consensus
configuration entries, leader removal and corresponding quorum/read changes are
next. All participating applications/backends must support snapshots before
compaction is enabled; no capability negotiation is implemented. Asynchronous/COW
capture, disk-streamed transfer, bounded transport queues, total heap quotas,
cluster/store authentication and independent review remain open.
The conservative backend still rewrites the entire snapshot and retained suffix
on each update, and capture/publication runs synchronously on the event loop.
