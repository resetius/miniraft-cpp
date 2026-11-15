# Non-voting learners: v0.1.0-alpha.17

Raft startup rejects a zero current term or a persisted vote for an ID outside
the configured fixed voter set, including on unbound nonfresh and legacy stores.
It performs this check before voter binding or serving traffic. This check does
not prove that the full voter set matches a legacy store's original cluster;
legacy storage still has no persisted voter binding.

This milestone separates replication targets from the fixed voting set. Learners
receive log entries, install snapshots and apply committed commands, but never
campaign, vote, lead, propose entries or contribute to election, commit or strong
read quorums. It implements the catch-up phase described in
[Raft section 6](https://raft.github.io/raft.pdf). Promotion, voter removal and
joint-consensus configuration entries remain the next milestone.

## Configure a learner

The KV CLI accepts repeatable `--learner id` options, requiring `--server --durable`.
All replicas must agree on the same voter IDs and role list. Include every voter
and learner endpoint in `--node`; mark each learner by ID on every process. For
example, with voters 1/2/3 and learner 4:

```sh
./build/full/examples/kv --server --id 4 --durable \
  --node 127.0.0.1:8001:1 --node 127.0.0.1:8002:2 \
  --node 127.0.0.1:8003:3 --node 127.0.0.1:8004:4 --learner 4
```

Use the same endpoints and `--learner 4` on voter processes, with their own IDs.
Learners can catch up from a compacted prefix even without local periodic
snapshotting. Clients can connect through them: writes and strong/default reads
are forwarded to the known voter leader; explicit stale reads use local applied
state. A learner does not elect a replacement leader when disconnected.

The constructor gains an optional fourth argument, the learner-ID vector.
Omitting it preserves all-voter interpretation. IDs must be positive, unique and
present among the local/peer IDs. At least one voter is required. Transport peer
count still includes learners; GetNservers/GetMinVotes describe voters/quorums.
Memory TState supports role binding for library tests/volatile deployments, but
only the durable backend preserves that binding across actual process loss.
Legacy TDiskState does not support learner mode.

The KV CLI uses static transport discovery. Library embedders can use
[online learner staging](online-learner-staging.md) on the current leader, and
`TRaftServer` can attach its `TNode` endpoint through
[network learner staging](online-learner-network.md). That local, non-voting
replication target is not persisted or cluster membership.
Deleting `--learner 4` to turn it into a voter is rejected for bound stores; it is
not a membership-change procedure. Durable Raft embedders can use the separate
[live joint-membership API](configuration-entries.md) after catch-up and commit
acknowledgement.

## Quorum and term isolation

Leaders replicate to every configured peer, tracking normal RPC-correlated
MatchIndex/NextIndex and snapshot progress. Commit advancement selects indices
only for explicit voter IDs. Election RPCs are sent only to voters, learner vote
responses are ignored, and strong-read confirmation collects only voter ACKs.
One voter with multiple learners can still commit/read alone; a voter minority
cannot make decisions merely because learners are available.

A learner acknowledges vote requests with a denial and never records a vote.
It follows legitimate voter terms but cannot advance its own term by election
timeout. Voters discard learner-origin vote/leader requests before term mutation.
Replication ACKs from learners are accepted only in the voter's current term, so
an outlier learner term cannot depose a healthy voter leader. A learner with an
incompatible future term may remain unable to catch up; this does not force the
voting quorum to adopt that term. This is a trusted-member protocol, not network
authentication against forged voter IDs.

IsLearnerCaughtUp(id) is a leader-only, point-in-time log watermark: the configured
learner must match the leader's current log end and the leader must already know
a current-term commit. It does not change roles, authorize promotion, attest to
application health or supply an application-applied watermark. Persistence of
ACKed entries depends on the selected storage backend. Unknown IDs and voter IDs
return false. A new unreplicated tail makes the status false again.

## Durable voter binding and upgrade

Fresh binding-capable stores record the voter IDs before accepting any vote or
replication traffic, including all-voter deployments. A snapshot-backed v2 store
can adopt exactly the voter set already recorded in its embedded snapshot. Bound
stores reject different voter IDs on restart and reject snapshots that would
replace that set. Learner local stores must not contain a vote. Ordinary role
validation failure does not modify term, vote, log or application data.

A nonfresh v1 store without an embedded snapshot has no trustworthy record of its
old voter IDs. Learner mode refuses to guess. First run it under its unchanged,
verified all-voter configuration and create an embedded snapshot with compaction;
then enable learner-aware discovery with the same voter IDs. Unbound legacy
all-voter operation retains its previous startup contract until that proof exists.
A standalone alpha.15 checkpoint alone is not an embedded configuration witness.
Never delete voter state or edit votes/terms to work around this validation.

Current durable images use `MRFTD004` and retain the 20-byte header. The payload
stores CurrentTerm[8], VotedFor[4], retained-log count[8], CommitIndex[8], voter
count[4], sorted voter IDs[4 each], snapshot length[8], optional MRFTS001,
MRFTS002, or MRFTS003 image, then retained log records. The CRC covers version
magic plus payload. Recovery bounds and validates the voter list, commit
boundary. Legacy snapshots must name the original bound voters; configuration-
bearing snapshots can describe a later active set while retaining that original
binding for offline history scans. Raft startup still requires the runtime voter
set and binding to match. MRFTD001 through MRFTD003 remain readable; formats
without commit metadata conservatively use SnapshotIndex as their commit floor.
The next successful publication writes v4
using the existing atomic write/fsync/rename/directory-fsync protocol. A crash
before first binding leaves either a fresh unbound store with no votes/log, or a
complete bound store. Publication failure is fatal before the node can serve
traffic.

Upgrade all voters before configuring any learners. Older binaries treat every
peer as a voter and cannot read v4 stores; mixed-role/mixed-version operation is
unsupported. All fresh bootstrap configurations must agree. Persistent binding
does not authenticate initial bootstrap choices or wrong-store copies. No cluster
UUID, role negotiation or automatic migration from unknown membership is added.

## Validation and review

Fifteen new deterministic unit tests cover malformed role lists, no learner
campaign/vote/proposal, term isolation, voter-majority arithmetic for sizes 1–8
with arbitrary learner matches, a single voter with several learners, ignored
learner grants, commit/read isolation during voter partitions, ordinary and
snapshot replication, log catch-up status, restart role preservation, rejected
manual promotion, learner-only transport expansion, unbound legacy rejection,
v2 snapshot-backed adoption, v3 corruption with valid checksums, unsupported
legacy storage, and crashes at all five first-binding publication stages.

A new real-process learner CLI test starts an offline learner after a compacted
two-voter majority, verifies local catch-up and tail application, restarts the
learner, confirms its persisted vote is zero, and checks that removing the role
flag fails without rewriting its store. The existing checkpoint/InstallSnapshot
smokes also run against v3 stores. Fault schedules are deterministic core tests;
CLI smokes use separately bounded socket/process waits.

Local validation passes 222 unit tests and three CLI tests under ASan/UBSan across
17 CTest targets; Release core checks pass. Self-review checked every quorum
site, pre-term RPC filtering, local-role guards, immutable voter binding, adoption
proofs, snapshot voter metadata, v1/v2/v3 decoding, publication ordering and the
distinction between log catch-up and promotion. No independent reviewer approval
is claimed. Process crashes do not substitute for hardware power-cut testing.

This closes the non-voting catch-up phase, not dynamic voting membership. Next
are replicated joint configurations, old/new election/commit/read majorities,
promotion gates, leader removal and configuration history in snapshots. Online
transport discovery, authenticated bootstrap, SQL snapshots, resource quotas and
independent review remain open production gates.
