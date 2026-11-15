# Joint snapshot metadata foundation: v0.1.0-alpha.25

The [alpha.27 follow-up](configuration-history.md) requires the configuration
term to equal the snapshot term when both refer to the same index.
The [alpha.48 follow-up](unsupported-snapshot-term-preflight.md) checks the
initial snapshot format before accepting a higher RPC term. The [alpha.49 follow-up](snapshot-metadata-term-preflight.md)
also checks the embedded boundary, fixed voters, and known stable configuration
identity.

This step adds an experimental snapshot representation for a joint configuration
effective at the included snapshot boundary. MRFTS002 records the old and new
sorted voter sets plus the configuration entry's index and term. Its index must
be positive and no greater than the snapshot index; its term must be positive and
no greater than the snapshot term. Both voter sets are nonempty, positive, sorted,
unique and individually bounded. The codec validates structure, not whether the
configuration was committed or belongs to an authenticated cluster.

The v2 byte layout is magic[8], snapshot index[8], snapshot term[8], old voter
count[4], application data length[8], configuration index[8], configuration
term[8], new voter count[4], old voter IDs[4 each], new voter IDs[4 each], opaque
application data, and CRC32[4] over all preceding bytes. Lengths and counts are
bounded before allocation. Fixed snapshots continue to encode as MRFTS001 with
exactly the previous bytes; a golden v1 image tests compatibility. The standalone
snapshot store can export and load v2 images for testing or offline tooling.

This is a format and offline persistence foundation, not live joint consensus or
complete configuration history. A bound memory or durable store can install and
recover a joint snapshot for offline validation while preserving its original
voter binding, which may differ from the snapshot's active old voter set after
earlier transitions. The caller must supply a trusted history baseline; the
snapshot codec and store do not prove that the transition was committed. Raft
startup and RPC paths still reject joint snapshots, and compaction only creates
fixed-voter snapshots. A covered
snapshot is acknowledged only after its prefix identifies supported fixed v1 or
stable v3 metadata, including when the prefix arrives in several chunks.
Unsupported joint images are rejected without replacing application data or
publishing a joint snapshot. Since alpha.48, a higher-term InstallSnapshot can
advance hard state only after its initial chunk identifies a supported MRFTS001
or MRFTS003 format. Since alpha.49, that chunk must also contain the complete
bounded metadata prefix and match the RPC boundary, fixed voter set, and any
known stable configuration identity. Short, unknown, joint-format, mismatched,
or continuation chunks cannot change the term before rejection. These rules
avoid falsely acknowledging a joint image merely because its index is already
committed locally.

Older binaries do not understand MRFTS002 and may acknowledge an already-covered
image without checking its version. Do not send or activate joint snapshots in a
mixed-version cluster. Future live use requires an upgrade of every voter, a
defined compatibility protocol, replicated configuration entries, durable and
snapshot recovery of configuration history, transition rollback rules and dual
quorum handling throughout the protocol. This experimental v2 format may change
before those semantics are implemented. A standalone export is not proof that a
configuration is committed and is not an authoritative Raft checkpoint.

Tests cover a golden v1 image, joint round-trip, every-byte corruption and
truncation, semantic corruption with valid CRC, bounds and ordering, standalone
publication, bound durable round-trip, runtime rejection without rewriting the
image, and network rejection both above and below the commit boundary. They also
cover a supported v1 prefix split across chunks. The snapshot struct's
in-memory layout changes; rebuild the library and dependents together. Self-review
checked version dispatch, bounds, immutable voter binding, covered acknowledgments
and fail-closed recovery. No independent reviewer approval is claimed.
