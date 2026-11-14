# Stable configuration snapshot runtime support: v0.1.0-alpha.46

The runtime now accepts MRFTS003 snapshots for the node's already configured
fixed voter set. Startup requires the snapshot voter list, supplied Raft peers,
and persisted voter binding to agree. Joint MRFTS002 snapshots remain rejected.

With that same-set check, the existing snapshot lifecycle can restore a durable
snapshot into the application, receive and persist it over InstallSnapshot, and
compact later application progress. Compaction carries the prior stable
configuration entry index and term into the new image. A follower whose committed
prefix already covers a stable snapshot validates the RPC boundary, voter list,
configuration identity and encoded data length before acknowledging it.

Once a node has a stable configuration identity, installing a newer MRFTS001
snapshot carries that identity into the stored MRFTS003 image. An incoming
MRFTS003 image with a different identity is rejected, including on the
already-covered fast path. This prevents a legacy-format refresh from silently
erasing configuration history and prevents a peer from replacing it with a
conflicting baseline.

This does not implement membership changes. Configuration log entries remain
disabled, the voter binding cannot change, and a stable snapshot claiming a
different voter list is rejected. Joint consensus, transition rollback and
snapshot installation across a membership transition remain open. Upgrade all
voters before sending MRFTS003 images because older binaries may acknowledge an
already-covered unknown snapshot format.

Tests cover durable application restore, identity preservation through
compaction, restart, and legacy-format refresh, chunked InstallSnapshot,
covered-prefix validation of stable metadata, and rejection of conflicting
identities and joint snapshots. Self-review completed; no independent reviewer
approval is claimed.
