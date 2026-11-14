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

This does not implement membership changes. Configuration log entries remain
disabled, the voter binding cannot change, and a stable snapshot claiming a
different voter list is rejected. Joint consensus, transition rollback and
snapshot installation across a membership transition remain open. Upgrade all
voters before sending MRFTS003 images because older binaries may acknowledge an
already-covered unknown snapshot format.

Tests cover durable application restore, identity preservation through
compaction and restart, chunked InstallSnapshot, covered-prefix validation of
stable metadata, and rejection of joint snapshots. Self-review completed; no
independent reviewer approval is claimed.
