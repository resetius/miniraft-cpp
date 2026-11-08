# Bounded network framing: v0.1.0-alpha.7

This milestone fixes fragmented TCP header handling and validates structural
frame bounds before allocation, typed casts or delivery to Raft.

## Reproduced failures

The reader assumed each four-byte header field arrived in one ReadSome call,
rejecting valid byte-fragmented TCP input. It trusted the declared length and
AppendEntries count before allocation, accepted unknown/unimplemented types and
short fixed-size messages, and recursively accepted arbitrary child messages.
All six initial deterministic regressions fail against alpha.6.

The message factory also only asserted an eight-byte allocation minimum even
when placement-constructing a larger derived type. Undersized typed allocations
could overwrite their buffers, particularly when assertions were disabled.

## Decoder and encoder contract

Headers and bodies now use exact-read loops. Before allocating, the decoder
checks the type, its minimum/fixed size, the frame cap and remaining batch
budget. Vote/AppendEntries control frames have exact sizes. Log entries and
command request/response frames have typed minimum sizes and variable data.
Unknown types and unimplemented snapshot types are rejected.

AppendEntries children must be log entries with no nested payloads. Entry count
and minimum required bytes are checked before allocating the holder array; every
child consumes the same aggregate byte budget. Nesting depth is bounded to the
root and one log-entry level. EOF/truncation is an error; a failed reader cannot
resume at a later frame. Caller adapters must discard the failed stream.

The writer validates the entire outgoing tree before its first byte, including
Nentries/PayloadSize agreement, required payload storage, child types and byte
budgets. Valid consecutive frames retain their boundaries under partial I/O.
The bundled server closes rejected inbound streams and resets failed outbound
connections after the reader unwinds, rather than parsing more bytes from the
damaged stream. Associated forwarded attempts receive the existing failure
notification; write outcomes may remain unknown.

## Default limits and replication progress

* Maximum frame size: 16 MiB, including its header.
* Maximum complete AppendEntries batch: 64 MiB, including root and child headers.
* Maximum AppendEntries child count: 4096.

Raft log proposals are checked against the frame cap before mutating the log or
the proposed entry's term. Batch construction now observes the same aggregate
cap and continues with the remaining entries after acknowledgement. This avoids
building a valid-looking replication batch that every peer would reject.

Application Prepare implementations can expand commands. If their prepared log
entry exceeds the cap, the proposal is rejected before append; an accepted
command-frame length alone does not guarantee its prepared entry fits.

TMessageLimits can be supplied to standalone reader/writer adapters. The bundled
Raft sender/admission path uses shared defaults; coordinated deployment-level
limit configuration is not introduced here. Stricter custom adapter limits can
reject the sender's batches. Existing stores with oversized entries are not
supported by the new default replication cap and require review before upgrade.
The wire layout and native-endian representation are otherwise unchanged.

## Typed message safety

Factories reject allocations smaller than sizeof(T), including in Release builds.
Cast checks null, declared minimum size and the concrete protocol type; Maybe
and IsEx no longer expose short or empty messages as larger structures. A generic
TMessage factory can still hold opaque bytes for validation. Local callers must
provide genuine buffers and keep frames immutable during I/O: mutable length
fields are not an allocation-capacity capability, and forged local holders are
outside this contract.

## Validation and self-review

Fourteen deterministic stream tests exercise one-byte fragmentation, all header
truncation offsets, fixed-frame body truncation offsets, invalid/oversized lengths,
unknown types, wrong/nested children, entry count and aggregate bounds, exact
budget boundaries, reader poisoning, pre-write validation and adjacent frames.
They use immediate fake I/O, with no sockets, sleeps or wall-clock dependencies.

Four core tests cover typed allocation bounds, checked casts, bounded multi-batch
replication progress and rejection before persistent-state mutation. An older
election test fixture was corrected to store vote requests as requests instead of
casting them to vote responses; it still checks the same expected wire bytes.

All 112 tests pass locally under ASan/UBSan, with core Debug/Release verification.
Hosted CI runs the same targets; its results are separate from local checks.
Self-review checked subtraction/aggregation bounds, non-recursive child shape,
pre-allocation validation, pre-write validation, batch continuation and failed
connection reset ordering. No independent reviewer approval is claimed.

## Remaining gaps

This is structural framing validation, not authentication or complete validation
of consensus fields. Sender/destination membership, index/term semantics and
application payload lengths (KV/SQL/custom commands) still need their own checks.
Per-connection caps do not bound aggregate memory across unlimited connections;
admission limits, deadlines and broader fuzzing remain open. Native wire encoding,
TLS lifecycle stress, retry deduplication and generated history checking also
remain separate work. No library-wide security or linearizability claim is made.
