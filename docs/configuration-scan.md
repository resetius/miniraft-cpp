# Offline configuration log scan

`ScanConfigurationHistory` validates a retained suffix starting at a trusted
configuration history at the snapshot boundary. It returns the history at the
caller-supplied commit index and at the end of the suffix. The two results make
it possible to distinguish the configuration in that committed prefix from a
later configuration record, including after a suffix is replaced. A malformed
entry anywhere in the suffix fails the entire scan; the scanner does not decide
whether a damaged tail may be truncated.

The caller must independently establish the baseline and commit index: the scan
does not prove that any entry is committed, recover a commit index, authenticate
the snapshot, or establish cluster identity. A fixed v1 snapshot does not identify
its latest configuration entry, so its configuration identity must come from
other trusted information. Experimental stable and joint snapshots contain that
identity and the scan checks that the supplied active configuration matches it.
The persisted voter binding identifies the original trusted set; configuration
metadata can describe an active set reached by later transitions. Legacy images
without configuration metadata still require the original and snapshot voter
sets to agree. The scan checks snapshot and log boundaries, voter history, terms,
configuration predecessors, and transitions.
It requires a consistent, externally serialized view of the state; it does not
mutate storage, apply entries, activate voters, or change Raft roles.
It also rejects a configuration entry that follows an earlier configuration
entry outside the supplied commit boundary. In particular, a stable successor
cannot make an uncommitted joint configuration disappear from the latest view.

For ordinary entries, the scan trusts term and flag metadata from the storage
backend, which must validate its own records. Memory and durable backends avoid
copying ordinary payloads; external backends using default metadata accessors may
read full entries. Configuration records are read and validated in full.

The standalone scanner remains read-only. The Raft runtime uses it to validate
a simulated AppendEntries suffix before accepting configuration entries and to
recompute roles after commit or suffix replacement. Live changes additionally
require durable commit-index persistence, the original voter binding and local
endpoints for every active voter. Joint snapshot installation and snapshot
transfer across a transition remain unsupported. Snapshot metadata alone does
not authenticate a transition.
