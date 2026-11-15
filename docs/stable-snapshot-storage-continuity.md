# Stable snapshot storage continuity: v0.1.0-alpha.54

The stable snapshot identity checks previously lived in Raft's
`InstallSnapshot` receiver. Callers that used `TState::InstallSnapshot` or
`TDurableState::InstallSnapshot` directly could replace a known MRFTS003
configuration identity with another stable identity, or refresh it with an
MRFTS001 image that erased the identity.

The storage backends now apply the same continuity rule as the receiver. A
legacy refresh inherits the previously stored stable configuration index and
term before it is published. A stable image with a different identity is
rejected before the in-memory state or durable file changes. The receiver and
both backends share one helper, so future callers cannot bypass the rule by
installing snapshots below Raft.

This preserves only the fixed-membership identity already trusted by storage. It
does not enable joint snapshot recovery, live membership changes, or membership
activation from an untrusted snapshot. Joint images remain explicit metadata and
are still available to the existing offline durable format tests.

Tests install a legacy refresh and attempt a conflicting stable replacement
through both memory and durable storage. They check metadata preservation,
rejection atomicity, and durable recovery after restart. Release and ASan/UBSan
network-enabled CTest passed 21/21 each. This change was self-reviewed; no
independent reviewer approval is claimed.
