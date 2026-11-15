# Stable snapshot metadata continuity: v0.1.0-alpha.47

A node that already stores an MRFTS003 snapshot has a stable configuration
identity tied to its persisted fixed voter set. A newer incoming MRFTS001 image
does not include that identity. The receiver now carries its trusted identity
forward before persisting the new image, so legacy-format transfer cannot erase
the baseline needed by recovery and later compaction.

Incoming MRFTS003 snapshots must retain the known configuration index and term.
The receiver rejects conflicting identities both when installing a newer image
and when acknowledging an image whose boundary is already covered. These checks
do not enable membership changes through snapshots: the node still requires the
same configured voters, and joint snapshots remain unsupported. Membership now
changes through the separate configuration log protocol.

The focused compaction test covers a covered-boundary conflict, a legacy-format
refresh followed by restart, and a conflicting newer stable snapshot. The change
was self-reviewed; no independent reviewer approval is claimed.
