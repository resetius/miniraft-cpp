# Forwarded request lifecycle: v0.1.0-alpha.6

This milestone separates client cookies from forwarding correlation and gives
forwarded attempts an explicit leader/term/peer lifecycle.

## Reproduced failures

Followers previously interpreted every nonzero client cookie as an already
forwarded request and rejected it. For zero-cookie requests they replaced the
cookie with an internal routing token, then returned that token to the client
instead of restoring zero. They also mutated the caller's request buffer and
retained forwarding routes when terms/roles changed, allowing late replies to
complete obsolete attempts.

## Routing contract

EForwarded is a dedicated internal request flag. Clients may use any uint32_t
cookie, including zero and UINT32_MAX. A follower copies a request, sets this
one-hop flag and assigns a distinct nonzero routing cookie. A forwarded request
that reaches another follower fails instead of being relayed again.

The routing record holds the original cookie, client endpoint, target peer,
leader ID and origin term. Responses are accepted only from that configured
outbound peer. Before delivery, the route is removed and the response is copied
with the original cookie restored. Caller-owned request/response buffers are
not changed. Duplicate, unknown, wrong-peer and unattributed responses cannot
complete another attempt.

Routes are rejected with ErrorCode=1 and their original cookie when the origin
changes term, leaves follower state, observes a different leader, or receives a
peer-unavailable notification. Client disconnect removes only that client's
routes silently. Late responses for removed routes are ignored. Routing cookies
are never reused within a processor, even after success, cancellation or peer
reconnection. Exhausting the 32-bit wire space rejects further forwarding rather
than wrapping; a larger session/correlation protocol remains future work.

Cancellation is local bookkeeping. A write already sent or buffered by the
transport may still execute. No automatic retry, rollback or exactly-once
execution guarantee is introduced. ErrorCode=1 can indicate an unknown write
outcome, so non-idempotent writes must not be blindly retried.

## Transport integration and compatibility

Custom adapters must call OnCommandResponse(response, peer), passing the same
configured INode object used for forwarding, and call OnPeerDisconnected(peer)
when the outbound connection is reported unavailable. The old one-argument
response call still compiles but intentionally ignores unattributed responses.
The included server supplies peer context and reports outbound read/unavailable
errors. Repeated failure notifications are idempotent.

Configured-peer context is a routing check, not cryptographic authentication or
Byzantine fault tolerance. Untrusted inbound client sockets are not accepted as
the outbound peer that owns a response. Write-side failure notification timing
still depends on the transport detecting/reporting connection loss; blackholed
connections without such notification have no deadline here.

Construct a new processor only with fresh transport sessions: the cookie counter
is per processor, and this milestone does not provide a durable cross-processor
session generation. Message struct sizes are unchanged, but EForwarded reserves
flag value 0x08. Upgrade custom adapters and all cluster nodes together; mixed-version
operation is not validated.

## Validation and self-review

Twelve forwarding tests cover full-range client cookies, zero-cookie restoration,
request-copy/one-hop semantics, out-of-order and duplicate replies, term and
candidate transitions, stale replies arriving before an application poll,
selective client disconnect, leader discovery, wrong/missing peer context,
error propagation and peer failure/reconnection with late replies. Nine of the
initial ten tests fail against alpha.5; the selective-disconnect baseline passes.

An additional three-node deterministic test forwards a write and strong read
through actual Raft replication/application, including reversed and duplicate
consensus traffic. Together with existing tests, all 94 pass locally under
ASan/UBSan, with core Debug/Release verification. Hosted CI executes the same
targets; its results are separate from local checks. There are no sleeps or
wall-clock dependencies in the new tests.

Self-review checked original-cookie restoration, copy ownership, distinct token
allocation, route invalidation before response delivery, source-peer matching,
disconnect selectivity and adapter call sites. No independent approval is
claimed. Access remains serialized; callbacks must not re-enter the processor.

## Remaining gaps

Durable request IDs/deduplication, deadlines, admission limits, a larger
session/correlation namespace, broader transport validation and generated
history/linearizability testing remain open. This change does not prove
library-wide linearizability or guarantee completion during quorum loss.
