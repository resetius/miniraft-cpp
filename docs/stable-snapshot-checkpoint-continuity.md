# Stable identity checks for checkpoint restore: v0.1.0-alpha.55

`RestoreSnapshot` can load a standalone checkpoint into the application when its
index and term match the local retained log. It previously checked the fixed
voter list but did not compare an MRFTS003 checkpoint's stable configuration
identity with the authoritative snapshot already stored by Raft. A checkpoint
with a conflicting configuration index or term could therefore replace
application state while Raft retained a different configuration baseline.

Restore now rejects an explicit stable identity that conflicts with the local
authoritative snapshot before validating or loading application data. Legacy
checkpoints remain usable because they make no stable identity claim. Joint
snapshots remain rejected by the fixed-membership runtime.

The regression test uses a checkpoint whose index, term and voter set match the
retained state but whose stable identity conflicts. It verifies rejection leaves
the application unchanged, then verifies a legacy checkpoint at the same
boundary can still restore.

Release and ASan/UBSan network-enabled CTest each passed 21/21. This change is
self-reviewed; no independent reviewer approval is claimed.
