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

For ordinary entries, the scan trusts term and flag metadata from the storage
backend, which must validate its own records. Memory and durable backends avoid
copying ordinary payloads; external backends using default metadata accessors may
read full entries. Configuration records are read and validated in full.

Configuration entries and joint snapshots remain rejected by the live runtime.
Bound in-memory and durable stores can retain stable or joint metadata snapshots
for offline validation while preserving the original voter binding. The scanner
still requires a trusted configuration baseline; metadata presence alone does
not authenticate a transition. Live membership activation, commit rules, and
snapshot installation across membership transitions remain future work.
