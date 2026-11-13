# Leader-only proposal admission: v0.1.0-alpha.20

TRaft::Append previously rejected learners but accepted proposals on voter
followers and candidates. This violated the log-matching assumption that a term
and index identify one command. A follower could append a local command in the
current term at the same index where its leader subsequently proposed another.
AppendEntries compares entry terms, so the follower retained its different
command and acknowledged it. Both commands could then be applied as committed.

The deterministic three-node reproducer showed the leader applying `valid` and
a follower applying `rogue` at the same committed index/term. Ordinary client
requests through TRequestProcessor already route proposals through the leader;
the missing check affected callers using the public low-level Append API.

Append now requires the locally known LEADER state as well as a voter role.
Nonleaders receive std::logic_error before entry validation, term assignment or
storage mutation. This includes a former leader after a higher-term message has
made it step down. A candidate's self-vote does not permit proposals. Existing
leader-side frame validation remains in place, and incoming replication still
uses the storage backend through the validated AppendEntries path.

Two regressions cover both state preservation and the observed consensus failure.
The first rejects initial-follower, candidate and stepped-down-leader calls,
checking all stored fields and the supplied entry term remain unchanged. The
second attempts the conflicting follower proposal, commits a valid leader write
under duplicate/reordered delivery, and checks application histories, successful
client completion and identical committed prefixes on all three nodes. Both
failed on the old code; the convergence test observed `rogue` instead of `valid`.

Validation: 230 unit tests and three CLI tests under ASan/UBSan across 18 CTest
targets, plus all 12 Release core targets. Self-review checked the guard's position
before mutation, all production Append call sites, learner admission, higher-term
stepdown and the distinction between local proposals and follower replication.
No independent reviewer approval is claimed. There are no wire or durable format
changes and no migration requirement.
Previously divergent logs are not detected or repaired by this admission guard.

The role check is not a quorum lease: an isolated leader may still accept entries
that remain uncommitted, and a successful Append return is only a local log index.
Callers must serialize Raft access and wait for committed application/client
completion as appropriate. Directly editing IState or forcing states through the
test-facing Become API is outside proposal admission. Joint consensus and voter
promotion/removal remain open.
