# Consensus RPC validation: v0.1.0-alpha.8

Validate complete RPCs before changing terms, votes or logs. Unknown senders,
wrong destinations, inconsistent leader/candidate IDs, invalid booleans,
impossible log metadata and malformed payloads are ignored. IDs must be positive;
peer configuration cannot include self, zero or null endpoints.

Log indices are limited to INT64_MAX to match IState's signed indexing API.
AppendEntries checks index addition and nonzero, nondecreasing entry terms no
greater than the RPC term. The whole batch is checked before mutation, including
conflicts with committed entries. Follower commit advancement is clamped to the
prefix matched by that RPC, while retaining already-established commit progress.
SetCommitIndex preserves uint64_t width. Election/log-index exhaustion raises an
overflow error before wrapping or changing the vote/log.

Each peer retains at most 256 outstanding AppendEntries sequence/end-index
records. Success must identify a retained RPC and exactly its sent end index.
Processing a response discards that record and older records, preventing duplicate
or reordered failures from repeating backoff. NextIndex never falls below known
MatchIndex+1. Evicted replies are ignored; newer retained replies/retries can
still make progress. Very delayed peers under sustained request load can lose
old correlation records; admission control remains future work.

Eighteen deterministic tests cover identity, index/term bounds, false/unsent
acknowledgements, reordering, duplicates, follower commit bounds, committed-prefix
protection, batch atomic rejection, configuration, bounded response history and
counter exhaustion. All initial fourteen fail against alpha.7. Existing fixtures
with log terms greater than their RPC term, or mismatched candidate/sender IDs,
were corrected while preserving their intended reconciliation/election cases.

All 130 tests pass locally under ASan/UBSan, with core Debug/Release verification.
Hosted CI runs the same targets. Self-review checked validation ordering, bounded
correlation ownership, prefix safety, full-width arithmetic and progress after
rejection/eviction. No independent approval is claimed.

These are consistency checks, not peer authentication or Byzantine tolerance:
an untrusted connection can still claim a configured sender ID. Authentication,
application payload validation, deadlines, admission limits, retry deduplication
and broader generated histories remain open. Custom adapters must preserve RPC
sequence numbers and destination IDs. The wire format is unchanged; stricter
validation intentionally rejects formerly tolerated invalid messages.
