# Legacy log-term validation: v0.1.0-alpha.14

TDiskState recovery checked file sizes, offsets and entry framing but did not
validate the selected entries' term values. It could accept zero terms, a log
newer than CurrentTerm, or a term sequence that went backward. Those invalid
values then fed Raft's vote freshness and log-matching decisions. The opt-in
TDurableState backend already rejected these term sequences on recovery.

Legacy recovery now validates every selected entry's term: it must be nonzero,
no greater than the stored CurrentTerm and no less than the preceding term.
Startup throws std::runtime_error without modifying any stored bytes. Equal
terms and gaps between terms are valid, including full-width uint64_t values.

The basic range check is also applied when reading an entry after startup. If a
live read detects a zero term or a term newer than the in-memory CurrentTerm, the
existing Fail path terminates the node. An ordinary exception would be caught by
the network layer and allow a node with detected storage damage to keep serving.
Full sequence ordering is checked at startup, not by scanning all neighbors on
each live Get.

Only the prefix selected by LastLogIndex is validated. Obsolete suffix bytes left
by logical truncation remain ignored and can be overwritten by a valid append.
There is no file-format or storage-interface change. Previously accepted stores
with impossible term metadata now require verified recovery or safe rebuilding;
the code does not guess corrections or reset the node's term/vote.

Six new tests cover zero terms at both selected entry positions, future terms or
stale current-term metadata, decreasing terms, valid repeated/skipped/full-width
terms, an invalid obsolete suffix followed by replacement, and fatal live-read
range violations. Four tests failed against the previous implementation; two
positive compatibility tests already passed. Rejection tests compare snapshots
of every storage file before and after startup to verify byte-for-byte preservation.
The live-read subprocess test is excluded on Windows.

Two old test fixtures appended terms newer than CurrentTerm and only updated the
current term afterward (or never did). They now persist CurrentTerm first,
matching TRaft's existing term-update ordering before AppendEntries application.
Direct storage users remain responsible for consistent fields and valid append
inputs; this change does not add a proposal validator to the legacy writer.

All 181 tests across 11 CTest targets pass locally with SQLite under ASan/UBSan;
the Release core suite passes as well. Self-review checked full-prefix traversal,
unsigned comparisons, validation before exposing an entry, catchable startup
errors versus fatal live errors, ignored suffixes and fixture ordering. No
independent reviewer approval is claimed.

This detects impossible metadata rather than all corruption. Valid-looking term
or payload changes remain undetectable without checksums. Legacy TDiskState still
requires one writer and lacks power-loss durability, atomic publication and
crash-safe replacement. It is not upgraded to TDurableState by this validation
change. Snapshots, safe migration and independent review remain production gates.
