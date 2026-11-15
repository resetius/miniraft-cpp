# Linux durable state: v0.1.0-alpha.3

## Commit recovery: MRFTD004

The current durable image stores `CommitIndex` alongside the current term, vote,
voter binding, snapshot and retained log. Recovery requires
`SnapshotIndex <= CommitIndex <= LastLogIndex`. Raft publishes a commit advance
before it acknowledges the corresponding AppendEntries request or exposes the
new boundary to application processing. Restart therefore replays committed
entries even when the application checkpoint trails them. Installed snapshots
advance the persisted commit floor to at least their included index, and storage
rejects truncation of committed entries.

MRFTD004 keeps the 20-byte file header: magic[8], payload length[8] and CRC[4].
Its payload is CurrentTerm[8], VotedFor[4], retained-log count[8],
CommitIndex[8], voter count[4], sorted voter IDs[4 each], snapshot length[8],
optional encoded snapshot, then retained log records. The CRC covers the v4 magic
and payload. MRFTD001, MRFTD002 and MRFTD003 remain readable; their commit floor
is conservatively initialized to SnapshotIndex, since those formats did not
record commit progress. Their next successful publication writes MRFTD004.

This closes commit-boundary recovery for `TDurableState`; it does not implement
live membership changes or joint consensus. The legacy `TDiskState` format does
not persist this field and cannot provide the same recovery guarantee.

TDurableState is an opt-in storage backend with synchronous durable publication.
It closes the missing file-sync and atomic-publication gap for new stores, under
the filesystem assumptions below. The legacy TDiskState backend is unchanged
and still does not provide power-loss durability.

## Publication protocol

Each Append, RemoveLast and Commit publishes one complete image containing the
current term, vote and selected log prefix:

1. Serialize the complete state into a temporary file in the same directory.
2. fsync the temporary file and check the result.
3. Rename the temporary file over the published image atomically.
4. fsync the containing directory and check the result.
5. Return to the caller, allowing Raft to acknowledge the update.

An older image is never overwritten in place. A crash before rename leaves the
previous published image; a process crash after rename exposes the complete new
image. An unacknowledged update may therefore appear after recovery, which is
allowed. No acknowledged operation returns before file and directory sync finish.
Startup syncs the selected image and directory again before serving requests.
If a crash/filesystem error leaves the published name missing, startup rejects
the store rather than inventing a new term or vote.

Live publication errors terminate the process, since the networking layer catches
ordinary exceptions and would otherwise keep an unsafe node running. An error
after rename has an uncertain outcome; clients must not assume it rolled back.
Startup errors are exceptions and do not fall back to older/temp images.

## Format and ownership

The image has version magic, payload length and CRC32. Recovery checks the
checksum before decoding entries and validates bounds, commit metadata, snapshot
identity and term ordering. CRC32 detects accidental corruption but does not
protect against malicious modification or all collisions.

The files are NAME.durable.ID, NAME.durable.ID.tmp and NAME.durable.ID.lock.
A nonblocking advisory flock holds the lock for the object's lifetime. Use one
object/thread to mutate a store, and do not delete/replace its lock while running.
Returned/appended entries are copied so external mutation cannot change the
selected state behind the publisher's back. Temporary files are never recovery
candidates; the next publication replaces a stale temp file while holding the lock.

An existing lock with no published image is rejected, including an interrupted
first initialization. This conservative choice can require operator recovery
even if the node had not acknowledged any updates. Do not delete an existing
store's files to force startup.

Legacy entries/index/state files with the same base name are rejected. There is
no automatic migration or downgrade. Use this backend for fresh clusters until
a separately validated offline migration tool exists; do not replace an existing
node's state with an empty store under the same identity. A corrupt main image is never replaced by a
seemingly valid temp image.

## Use

```cpp
#include <miniraft/durable.h>
auto state = std::make_shared<TDurableState>("state", nodeId);
```

On Linux, the KV example accepts --durable in place of --persist. Run it in a
dedicated directory without legacy state files. --persist and the SQL example
continue to use the legacy backend. Other platforms build the library but
TDurableState construction throws an explicit unsupported-platform exception.

## Validation and self-review

Tests cover publication ordering, process-crash boundaries across
append/truncation/vote/replacement/commit updates, immediate exits after returned
updates, interrupted initializations, a live injected failure, an actual
write error using a child-only file-size limit, corrupted/truncated images,
single-writer exclusion, missing images, legacy-store refusal, entry ownership and Raft election/application replay across
a storage restart.
There are 31 child-process scenarios; none relies on sleeps or random scheduling.

All 61 tests pass locally under ASan/UBSan, and the core suite passes in Debug
and Release builds. These process-crash tests do not simulate physical power
loss or prove a storage controller honors flush commands. Hosted CI executes the
same targets; its results are separate from local validation.

Self-review checked sync-before-rename-before-ack ordering, suffix replacement,
directory durability, lock lifetime, failure propagation, format parsing and
recovery's refusal to adopt temporary images. No independent approval is claimed.

## Limits and assumptions

This first implementation rewrites the entire log on every update, uses memory
proportional to the log, and blocks the calling thread during sync. It establishes
a conservative protocol baseline, not NuRaft-level throughput. An efficient WAL,
group commit, snapshots and compaction remain necessary for substantial workloads.

The durability contract assumes Linux, a pre-existing durable parent directory,
and a local filesystem/storage stack honoring fsync and same-directory atomic
rename, with exclusive application ownership of the files. Network filesystems,
arbitrary external file deletion, broken hardware/flush semantics and real power
cut testing are outside this milestone. CRC32 is not a substitute for backups.
Storage durability also does not establish end-to-end linearizability or exactly
once application behavior; those consensus/client audits remain open.
