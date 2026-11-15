# Snapshot transfer pinning: v0.1.0-alpha.19

Periodic compaction could starve snapshot catch-up even after alpha.18 fixed
retry correlation. Each send selected the leader's newest snapshot and reset the
peer's transfer if its index changed. When compaction outpaced a multi-chunk
transfer, the receiver repeatedly started new images without completing one.
This affected both voters and non-voting learners.

Each peer now pins the immutable image and encoded bytes selected at the start
of its transfer. New local snapshots do not change its index, offset, content or
outstanding chunk identity. A completed transfer releases that image. If the
leader compacted past the receiver's next index, the next transfer starts from
the newest available image; otherwise ordinary AppendEntries supplies the tail.
An ACK for the older image proves only that image's boundary, never the newer
prefix. Snapshot ACKs still cannot confirm strong-read barriers.

Failed chunks restart the pinned image from offset zero. Leadership changes still
discard outgoing transfer contexts. One image/encoding pair is retained per
active peer transfer, with sharing between transfers of the same image. The
[alpha.43 pin budget](snapshot-pin-budget.md) later capped active retention to two
distinct image generations and defers further compaction until a transfer
completes. It does not add a byte quota for all Raft, application, transport and
temporary snapshot memory, or a general transfer cancellation API.

A deterministic regression commits and compacts new data before every chunk ACK,
including the final ACK, for both voter and learner receivers. The old code fails
on the first replacement (snapshot 5 instead of 4). The fixed code completes the
original image without resetting offsets, releases its snapshot object, then
installs the newer image and replicates a committed tail. Existing tests cover
timeouts, read traffic, duplicate/stale replies, receiver restart, leader change,
corruption, atomic durable publication and strong-read isolation.

Sanitizer CI also exposed a second reconnect deadline race after alpha.18's
generation checks. The dependency's finite-deadline connect registers both I/O
and timer wakeups. After timeout, a late I/O event can resume the retry sleep;
that sleep's timer may remain queued even after the connector finishes. Destroying
the completed connector then leaves a timer pointing to freed memory. Preserving
active connectors alone cannot prevent this case.

MiniRaft now calls the dependency's default connect operation, which registers no
connect deadline timer. TCP connection failure timing follows the operating
system, so a failed or blackholed connection may take longer than the former
100 ms application deadline. A safe bounded application-level connect deadline,
including cancellation of both wakeup sources, remains open. The coroio dependency
is unchanged. This is a targeted avoidance of the unsafe deadline path, not a
general asynchronous cancellation or shutdown implementation.

A deterministic lifecycle regression models a second deadline wakeup surviving
completion and disconnect; it reproduced the ASan use-after-free with the former
finite-deadline call and passes with the default connect operation.

Validation: 228 unit tests and three CLI tests across 18 CTest targets under
ASan/UBSan, plus 12 Release core targets. Self-review checked image ownership,
encoding reuse, completed-boundary accounting, follow-up transfer selection and
term replacement, connection wakeup sources and deadline behavior. No independent
reviewer approval is claimed. Wire messages and
durable formats are unchanged; no additional migration is required.

This guarantees progress of the selected finite image under eventual chunk
delivery and ACKs. Reaching the live log end still requires enough replication
capacity relative to writes/compaction and sufficiently stable leadership.
The bounded promotion and joint-consensus path is described in
[live membership](configuration-entries.md). Membership-aware snapshots and
leader removal remain open.
