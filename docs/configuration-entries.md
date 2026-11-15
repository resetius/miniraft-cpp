# Configuration entry foundation: v0.1.0-alpha.26

This milestone defines an experimental representation for a joint or stable
configuration log entry. It does not enable live membership changes. Each record
contains its phase, entry term, previous configuration entry index and term, and
the old and new sorted voter sets. The stable record carries both sets as context;
future transition logic must decide how to validate and apply it.

The MRFTC001 payload is magic[8], phase[4] (joint=1, stable=2), entry term[8],
previous configuration index[8], previous configuration term[8], old count[4],
new count[4], old voter IDs[4 each], new voter IDs[4 each], and CRC32[4] over
the preceding payload. The entry's outer term must equal the encoded term, and
its flags must equal EConfiguration, the high bit of the 64-bit flags field.
Counts, lengths, ordering and bounds are checked before allocation. A joint
entry may refer to an implicit bootstrap configuration with zero previous index
and term; a stable entry requires a nonzero predecessor. The codec checks shape,
not log position, predecessor identity, commitment, role eligibility or cluster
authentication. CRC is an accidental-corruption check, not authentication.

Current Raft rejects entries carrying the reserved bit in local proposals and
incoming AppendEntries, before changing term or log. Startup scans the retained
suffix and rejects such entries before voter binding or serving traffic. If a
configuration record reaches committed replay in a controlled test or future
runtime, the request processor validates it, advances its applied index, and
does not pass it to the application state machine. This no-op application rule
does not activate the configuration. Joint snapshots remain rejected as described
in [joint snapshot metadata](joint-snapshots.md). The codec and storage backends
can represent records for deterministic testing and offline tools, but they are
not an active replication or recovery protocol.

Compatibility matters: previous versions allowed custom entry flags. Before
upgrading, audit applications and retained logs for use of the high bit. A node
with that bit in its retained suffix now refuses startup; this release does not
rewrite or migrate it. A fixed snapshot may already contain effects of older
entries, which this suffix scan cannot detect. Older binaries may apply an unknown
configuration entry as application data, so no future version should replicate
these entries until every voter runs a compatible implementation. Do not delete
or edit committed state to bypass rejection.

The optimized memory and durable backends scan flags without copying payloads.
External and legacy backends use the default Get-based fallback, so startup can
read the full retained log; the interface's vtable changed and dependent binaries
must be rebuilt. Backends that override Get with different semantics must make
RetainedLogFlags consistent. No existing durable file or wire frame layout changed.

Tests cover both phases, bootstrap and maximum-size records, every-byte corruption
and truncation, semantic corruption with valid CRC, malformed frames, rejected RPCs
and proposals, application isolation, compacted suffixes and restart rejection
without rewriting durable or legacy files. Self-review checked fail-closed ordering,
the flag scan, predecessor shape and compatibility limits. No independent reviewer
approval is claimed. Dynamic promotion/removal, configuration activation and
rollback, joint snapshot recovery, discovery and full configuration history remain
open; this experimental encoding may change when those semantics are implemented.
