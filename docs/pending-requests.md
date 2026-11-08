# Pending request lifecycle: v0.1.0-alpha.5

This milestone prevents pending writes from receiving a replacement command's
reply after leadership changes. It also releases disconnected clients and
unused application replies, and makes pre-election requests progress when the
local node becomes leader.

## Reproduced failure

A leader accepted an uncommitted command at index 3. After a partition, another
leader replaced that index with a different command and committed it. When the
old leader rejoined, its request processor returned the replacement command's
successful reply to the original client. Requests and application replies were
matched only by index, with no leadership term or lifecycle validation. A
replacement no-op could instead leave no application reply and hit an assertion.

## Completion and cancellation contract

Local writes, default leader reads and strong reads now belong to the leadership
term in which they were admitted. Losing leadership or changing term rejects
pending operations with ErrorCode=1 and the original cookie. Re-election cannot
redeem an earlier term's request. Pruning runs before admission, application,
completion and leader-wait queue processing; unchanged terms/roles avoid rescans.

Write application replies are retained only while a matching local request is
pending. Both index and term must match before a success is returned. Replies
for follower/background application are released immediately. Completion,
leadership cancellation and disconnect cleanup remove cached replies and their
pending-write records. The retained reply cache is bounded by pending writes,
although admission limits for the request queues themselves remain open.

Cancellation concerns the response, not the replicated command. Committed
commands still apply even if their client disconnected or their leader changed.
An error can therefore mean UNKNOWN OUTCOME: the write may already have
committed. It is not a rollback indication and must not trigger a blind retry of
a non-idempotent operation. Request IDs and replicated deduplication are not
implemented. The KV, SQL and debug clients now display errors rather than
printing a successful commit for nonzero ErrorCode responses.

CleanUp removes a disconnected endpoint from ordinary, strong-read and
leader-wait queues, plus forwarded-request routing. Disconnect cleanup sends no
reply and does not retract entries already appended to the log.

Explicit stale reads are independent of leadership terms and target the known
committed prefix, rather than an uncommitted tail. Default reads remain weaker
than strong reads but are cancelled if their admitting leadership ends. FIFO
queueing can still delay a stale read behind an earlier pending request.

## Leader discovery and timer progress

A newly elected leader records its own ID. Higher-term stepdown clears stale
leader routing until AppendEntries identifies the new leader. Requests waiting
for leader discovery can execute locally after election, including a single-node
election. Invalid or self-directed follower routes remain queued rather than
asserting or dereferencing a missing peer.

The server's idle timer now runs request-state checks, committed application and
completion processing after Raft timeouts, using the same ordering as inbound
events. Progress therefore does not require an unrelated incoming message.

## Validation and self-review

Twelve new deterministic tests cover replacement by another command or no-op,
stepdown before application, already-committed but unanswered writes, re-election
without intermediate application polls, default reads of lost tails, requests
queued before single/three-node elections, disconnects from all local queues,
cached reply release, unused follower replies and stale reads of a committed
prefix. Weak references verify ownership is actually released. The initial ten
tests all fail against alpha.4.

All 81 tests pass locally under ASan/UBSan, with core Debug/Release verification.
Hosted CI executes the same targets; its results are separate from local checks.
The wire format and public request-processor methods are unchanged.

Self-review checked index-and-term matching, reply-cache bounds, cancellation
before log application, disconnect ownership, leader routing and timer/inbound
ordering. Access must remain serialized and callbacks must not re-enter the
processor. No independent reviewer approval is claimed.

## Remaining gaps

Exactly-once retries, durable request IDs, forwarded-request lifecycle across
leader failures, queue admission limits, deadlines, transport validation and
broader generated history/linearizability testing remain separate work. This
change does not promise successful completion during quorum loss or establish
library-wide linearizability. Unanswered writes after process crashes also have
unknown outcomes.
