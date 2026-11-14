# Snapshot metadata term preflight: v0.1.0-alpha.49

Alpha.48 checked a higher-term InstallSnapshot's magic before accepting its
term. That left one metadata gap: MRFTS001 or MRFTS003 could have a boundary,
voter list, or stable configuration identity that the receiver would later
reject, after its hard state had already advanced.

The higher-term preflight now uses the bounded metadata probe before term
processing. The initial chunk must contain the full format header and voter
list. The probe checks that the embedded index and term match the RPC, voter
count and IDs match the receiver's fixed membership, the encoded lengths fit the
declared total, and any existing stable configuration identity is preserved.
The largest metadata prefix is 16,436 bytes, within the sender's 64 KiB initial
chunk. Payload bytes and CRC remain the full install path's responsibility.

Malformed or conflicting metadata is rejected before changing the term, vote,
or leader state. Same-term handling remains unchanged. This does not install
joint snapshots or enable voter membership changes; those still require
transition-aware recovery and quorum rules.

Regression tests cover higher-term joint images, wrong voters, conflicting
stable identities, short or continued chunks, and same-term rejection. Release
and ASAN network-enabled CTest passed 21/21 each, and hosted build, Debug,
Release, sanitizer, and unit-test checks passed.

Self-review completed. No independent reviewer approval is claimed.
