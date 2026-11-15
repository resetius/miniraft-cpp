# Snapshot retry progress: v0.1.0-alpha.18

Follow-up: partial acknowledgements must report exactly the end of the
outstanding chunk. Previously a larger offset could skip unsent bytes. An
invalid acknowledgement now leaves the chunk outstanding for retry; a
deterministic regression verifies retry and eventual installation.

The [alpha.19 follow-up](snapshot-pinning.md) also pins an active transfer across
local compaction, replacing the image-switching behavior described below.

Timeout retries and strong-read requests could indefinitely stall InstallSnapshot
catch-up for a healthy follower or learner. Each send replaced the outstanding
chunk's sequence number. If an ACK arrived after another send, the leader rejected
it, retransmitted the same offset with another sequence number, and repeated the
cycle. This could happen without any corrupt messages or failed storage.

Retries now preserve the identity of the same immutable chunk. A valid partial
ACK consumes that identity; the next chunk receives a fresh sequence number. A
failure also consumes it before restarting from offset zero. Duplicate ACKs stay
stale both before and after the next send. A changed snapshot or leadership term
continues to discard the old transfer context. Invalid replies do not consume the
outstanding identity.

This reuse applies only to snapshot chunks. Snapshot ACKs still cannot confirm a
strong-read barrier; a subsequent fresh AppendEntries ACK is required. No wire
layout, durable format, role configuration or migration procedure changes.

Two deterministic regressions run against both voter and learner receivers. Each
ACK is held while the leader emits three retries, driven by either timeouts or
repeated ApproveRead calls; retry packets are dropped and the original ACK is
then delivered. Before the fix both tests failed at the first 64 KiB boundary:
the leader retransmitted offset zero instead of advancing to 65536. After the fix
the multi-chunk image completes, the application loads once, duplicate ACKs do
not consume subsequent chunks, and a new committed tail reaches the receiver.

The release also fixes a reconnect use-after-free discovered by the sanitizer CI
snapshot CLI test. A writer could start reconnecting before the old reader
reported its failure. Disconnecting that reader destroyed the pending connector,
but its socket connect timer still held the coroutine handle. When the timer
fired, it resumed freed memory. Disconnects now identify the reader's connection
generation and preserve active connectors; an old reader cannot destroy a newer
pending or established connection. This is not a general asynchronous shutdown
or cancellation API.

Two deterministic node lifecycle tests use the real node wrapper and poller with
a controllable socket awaiter. They cover disconnect during a pending connect
timer and an old reader failure racing a writer-triggered reconnect, both before
and after replacement establishment. The first reproduced the same ASan
use-after-free before the fix. Node template definitions are now in node_impl.h
so alternate sockets can exercise the production implementation without real
network timing. The coroio dependency is unchanged.

The full validation set is 226 unit tests plus three CLI tests across 18 CTest
targets under ASan/UBSan, with 12 Release core targets. Existing tests cover
partial-transfer restart, stale replies after a reset, leader replacement,
duplicate installation, corruption, durable publication crashes and strong-read
isolation. Self-review checked connection generation ownership and outstanding
connector lifetime as well as chunk identity lifetime. No independent approval
is claimed.

Snapshot catch-up is a prerequisite for promotion, not authorization by itself.
The bounded durable joint-consensus API is described in
[live membership](configuration-entries.md); leader removal and
membership-aware snapshot transfer remain open.
