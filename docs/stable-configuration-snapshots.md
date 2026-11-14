# Stable configuration snapshot metadata

The experimental MRFTS003 snapshot format preserves a stable voter set and the
index and term of the configuration entry that selected it. This fills a gap in
MRFTS001: its voter list does not establish the identity needed to validate a
later configuration entry. MRFTS003 stores magic[8], included index[8], included
term[8], voter count[4], application length[8], configuration index[8],
configuration term[8], voter IDs[4 each], application data, and CRC32[4]. The
configuration identity must be nonzero, no later than the snapshot boundary, and
have the same term when both indices coincide. Voters are sorted and bounded.
MRFTS001 and MRFTS002 encoding is unchanged.

`TConfigurationHistory::FromStableSnapshot` initializes offline history from
this identity. The retained-log scanner checks the baseline identity against
the snapshot before checking subsequent joint/stable entries. These checks do
not prove commitment or authenticate the snapshot: the caller must establish a
trusted snapshot and commit boundary independently. MRFTS003 cannot assert that
there was no earlier configuration entry; an explicit trusted baseline remains
necessary for such histories.

The runtime accepts MRFTS003 only when its voter list matches the node's existing
fixed voter binding. It can recover the image at startup, restore the application,
install it from a peer, and preserve its configuration identity through later
compaction. The covered-snapshot path checks the stable configuration identity
and voter list before acknowledging an already-covered boundary. These paths do
not activate a changed voter set or enable joint consensus.

Older binaries may falsely acknowledge covered unknown formats; do not send these
images to a mixed-version cluster. Joint snapshot installation, live
configuration activation, rollback, commit rules and membership recovery remain
open. This experimental format may change before those semantics are implemented.
Rebuild dependents because the in-memory snapshot structure has changed.
