# Unsupported snapshot term preflight: v0.1.0-alpha.48

Before this change, `TRaft::Process` persisted a higher RPC term before the
InstallSnapshot handler had read enough bytes to recognize its format. A joint
MRFTS002 image is intentionally unsupported, but its first chunk could still
advance the follower's durable term before the handler rejected it.

For a higher-term InstallSnapshot request, the RPC validator now requires an
initial chunk with at least the eight-byte magic and accepts the term only when
that magic identifies a supported MRFTS001 or MRFTS003 image. Higher-term
continuation chunks are rejected because a term change discards partial
transfers. Current-term malformed or joint images continue through the ordinary
fail-closed rejection path. The outbound sender already places the complete
magic in its initial chunk.

This closes the hard-state mutation gap for snapshots that can be recognized as
unsupported from their first chunk. It does not enable joint snapshot transfer,
joint snapshot recovery, or live membership changes.

The compaction regression test sends a full joint image, a short first chunk,
and a continuation chunk at a higher term, then verifies that term, vote, log,
snapshot and responses remain unchanged. It also verifies the same-term rejection
path. Release and ASAN network-enabled CTest passed 21/21 each; hosted C++ build,
Debug, Release, sanitizer, and test-result checks passed.

Self-review completed. No independent reviewer approval is claimed.
