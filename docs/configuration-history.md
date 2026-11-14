# Offline configuration history validation: v0.1.0-alpha.27

`TConfigurationHistory::MakeSnapshot` projects the history at its through-index
and term into an offline snapshot. It selects MRFTS002 for a joint configuration,
MRFTS003 for a stable configuration with a known configuration entry, and the
legacy MRFTS001 format when the trusted baseline asserts no earlier configuration
entry. That legacy format does not encode or prove the zero-identity assertion.
The caller must independently establish that the history boundary is committed
and that the supplied application image corresponds exactly to that boundary.
The method validates snapshot metadata and size; it does not capture application
state, compact or mutate storage, establish commitment, or activate voters.
Joint configuration snapshots remain rejected by the live runtime. Stable
metadata snapshots can be recovered and installed when they match the node's
existing fixed voter binding; this does not enable voter changes.
Bound storage can retain and recover stable or joint metadata snapshots for
offline validation, provided the snapshot voter list matches the store's existing
voter binding.

TConfigurationHistory checks a sequence of retained log entries against a trusted
configuration baseline. It joins the experimental configuration-entry codec with
the joint-snapshot metadata and checks that joint and stable records have the
expected predecessor index/term and voter sets. A stable configuration can advance
to a joint one; that joint configuration can advance to a stable record naming the
same old and new sets. A successful stable transition selects the new voters.

The caller visits every retained entry in order: Observe handles ordinary entries,
and Advance handles configuration entries. Both return a new history value and
leave the original unchanged on success or failure. They reject index gaps,
nonpositive or decreasing terms, malformed records, wrong predecessors, wrong voter
sets and invalid phase transitions. A caller can retain a previous value and scan
a replacement branch from that point. This models validation of rollback; it does
not truncate a live log or decide when a configuration becomes active.

FromJointSnapshot validates the snapshot metadata and starts at its included
index and term, using its joint configuration identity. Stable accepts an explicit
configuration identity and scanned-prefix boundary. The caller must establish that
baseline independently: a v1 fixed snapshot contains voter IDs but does not prove
the latest configuration entry's identity. Passing a zero identity asserts that no
earlier configuration entry exists. Decode and authenticate any persisted input as
appropriate before supplying it. The validator does not prove commitment, cluster
identity, promotion readiness, log completeness outside the supplied sequence or
the correctness of the caller's trusted baseline.

The [alpha.53 follow-up](configuration-scan-identity.md) ensures that the scan's
uncommitted-configuration result compares phase and voter sets as well as
configuration index and term.

Joint snapshot metadata now rejects a configuration term different from the
snapshot term when both indices are equal: one log index cannot have two terms.
Earlier alpha.25 tooling could encode such structurally inconsistent v2 images;
they are now rejected and are not rewritten automatically. The live runtime never
accepted joint snapshots. Fixed v1 snapshot encoding and existing durable formats
are unchanged.

Tests cover joint/stable chains, alternate branches, joint snapshot boundaries,
invalid baseline identities, skipped entries, stale terms, wrong sets and
predecessors, malformed entries, index exhaustion and CRC-valid snapshot metadata
corruption. A durable test scans configuration entries after a fixed snapshot and
restart, confirms the fixture's known bootstrap baseline, and verifies that Raft
still rejects the unsupported entries without rewriting storage. Full tests also
cover existing snapshot installation and learner catch-up.

This is configuration history validation for fixed membership, not live
reconfiguration. Joint entry activation and rollback, snapshot installation
across membership transitions, promotion gates and transport discovery remain
open. Self-review
checked contiguous scanning, term and predecessor relationships, immutable branch
results, snapshot boundary consistency and the unknown-v1-identity case. No
independent reviewer approval is claimed.
