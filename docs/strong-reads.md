# Fresh-quorum strong reads: v0.1.0-alpha.4

This milestone fixes a reproduced stale-leader read bug in EConsistent and the
KV example's --consistent mode. Default reads and explicit stale reads keep
their previous weaker semantics.

## Reproduced failure

In a five-node cluster, node 1 had previously replicated its log to all peers.
A partition left it connected only to node 2. Nodes 3, 4 and 5 elected a new
leader and committed another entry. A strong read arriving at node 1 then
received a successful stale reply after one fresh response from node 2.

The implementation combined historical MatchIndex values with the sequence
number of a single fresh response. Replication quorum was incorrectly treated
as proof that a fresh quorum had confirmed this read's leadership.

## Read protocol

Read confirmation now has per-peer acknowledgement watermarks separate from
log replication indices. A strong read reserves a unique nonzero uint64_t
barrier, then sends AppendEntries RPCs whose sequences are newer than it.
Successful responses from configured peers advance their individual watermarks;
duplicate replies cannot add voters. Pre-read replies cannot reach the new
barrier. Sequence allocation checks exhaustion rather than wrapping to zero.

The confirmed watermark is the strict-majority order statistic of the leader's
own sequence and each peer's acknowledged sequence, including zero for peers
that have not replied. This works for both odd and even cluster sizes. A
five-node read therefore needs the leader plus two distinct fresh peers.

Confirmation is usable only while the node is leader and an entry from its
current term is committed. The local state machine must also have applied the
read's target log prefix before executing the read. The target remains the
last local log index at read admission, a conservative choice that can wait
behind an uncommitted tail rather than optimizing read latency.

Each pending strong read records its leadership term. Losing leadership or
changing term rejects it with ErrorCode=1 and its original cookie. This is a
conservative cancellation policy; operations are not automatically replayed.
Term checking also works if the application does not poll between stepping
down and being elected again. A later read cannot inherit an earlier read's
quorum. If EConsistent and EStale are both set, EConsistent takes precedence.

The argument assumes a fixed configured voter set, correct non-Byzantine peers,
and serialized access to Raft/application state. It uses RPC ordering rather
than clock leases. It does not establish complete end-to-end linearizability
for the library or application.

## Validation and self-review

Eight new deterministic tests cover:

* Fresh-majority thresholds for cluster sizes one through eight, with duplicate
  responses (including single-node progress).
* The five-node stale-leader partition and a new majority's committed write.
* A second read requiring fresh confirmation after a previous read completes.
* Leadership loss after confirmation but before application service.
* Conflicting consistent/stale flags.
* Delayed pre-read replies, followed by partial and complete fresh quorums.
* A new leader waiting for its current-term commit and local application.
* An old-term read rejected after stepping down and being re-elected, before
  accepting a new read.

The first five tests all fail against alpha.3. The existing single-node FIFO
test now elects a real leader and commits its initial no-op, satisfying the
strong-read protocol's preconditions instead of merely forcing the role flag.
All 69 tests pass locally under ASan/UBSan, with core Debug/Release verification.
Hosted CI runs the same targets; its results are separate from local checks.

Self-review checked distinct-voter counting, strict-majority arithmetic,
monotonic nonzero barriers, term isolation, duplicate/delayed replies, current
term commitment and application progress. No independent approval is claimed.

## Compatibility and remaining work

The wire layout is unchanged. TRaft::ApproveRead and TRaft::CommitSeqno remain;
CommitSeqno now reports a fresh-majority read watermark rather than the largest
sequence attached to a log acknowledgement. Internal TVolatileState removes
CommitSeqno, adds acknowledgement maps, and CommitAdvance no longer accepts a
sequence argument. This alpha does not promise internal-structure or mixed
version cluster compatibility.

Pending write/default-read handling across leadership changes, disconnect
cleanup, retry/deduplication, admission limits, transport validation and broader
generated history/linearizability testing remain separate gaps. Quorum loss can
leave a strong read waiting indefinitely; deadlines and cancellation APIs are
not introduced here.
