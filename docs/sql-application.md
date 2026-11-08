# SQL transaction and progress handling: v0.1.0-alpha.11

The previous SQL write path concatenated client text, progress metadata and COMMIT
into one script. Client COMMIT/END could publish a prefix before a later statement
failed; metadata update errors were ignored on the failure path. Rejected SQL
also left LastAppliedIndex stale in memory even when its persisted marker advanced.
Restart used stoi, rejecting indices above INT_MAX and accepting malformed numeric
prefixes. Regression tests reproduced these failures before the fix; the old test
run aborted when the full-width-index restart threw out_of_range.

Writes now begin an IMMEDIATE transaction and execute the client script separately
under a SQLite authorizer. The policy blocks transaction/savepoint controls,
PRAGMAs, ATTACH/DETACH, temporary objects, virtual-table creation/deletion and
ALTER TABLE. It permits
ordinary main-database DML, table/index/view/trigger creation and deletion, REINDEX,
ANALYZE and the same function allowlist as reads. This deliberately narrows the
previous unrestricted SQL interface. Scripts may still contain multiple ordinary
statements, and comments cannot swallow internal metadata SQL.

Payload access to raft_metadata_ is denied, including through compiled views and
triggers. Payloads cannot create triggers or indexes on that table. The authorizer
is removed before the application's own metadata and transaction statements run.
The policy stays active throughout payload execution and automatic recompilation.
See SQLite's [authorizer contract](https://www.sqlite.org/c3ref/set_authorizer.html).

Successful data changes and their progress marker commit in the same transaction.
SQLITE_ERROR, AUTH, CONSTRAINT and MISMATCH are treated as application rejections:
all payload effects are rolled back, then a checked transaction records that the
entry was consumed. The original SQL error text is returned using the existing
write reply format. Memory advances only after COMMIT succeeds. An already applied
index is skipped; original replies are not persisted for retry deduplication.

Other execution errors, and any BEGIN, rollback, metadata or COMMIT failure, throw
without advancing memory. The committed-entry handler terminates on such an
exception, allowing restart/replay rather than acknowledging uncertain application.
A scope guard attempts rollback during unwinding. SQLite can itself roll back a
transaction, so cleanup checks [autocommit state](https://www.sqlite.org/c3ref/get_autocommit.html)
before issuing ROLLBACK. A crash between rolling back a rejected payload and
recording its marker can replay that rejection; no payload effects survived.

Startup checks initialization and progress queries, closes the database on
constructor failure, and parses the complete stored text as uint64_t. NULL, empty,
negative, overflowing or partially numeric markers (including embedded NULs) are
rejected. An absent marker continues to mean zero for legacy empty databases;
this does not detect a marker deleted by external tampering or previous releases.

Ten new tests cover transaction escape, direct/indirect metadata access, rollback
and restart, trailing comments, OR ROLLBACK, metadata-update failure, full-width
indices, malformed metadata, lock failures at BEGIN and COMMIT, and NUL rejection
before admission/preparation. All 162 tests across 11 CTest targets pass locally
with ASan/UBSan and SQLite; the Release core suite passes as well.

Self-review checked authorizer arguments and lifetime, separate internal SQL,
rollback/automatic-rollback behavior, commit ordering, error classification,
constructor ownership and bounded full-width parsing. No independent reviewer
approval is claimed. These are deterministic logical/locking failure tests,
not new power-loss or filesystem fault-injection coverage for SQLite.

This remains an experimental example using trusted SQLite builds/modules and a
private serialized connection. Existing unsafe schemas or legacy log scripts need
review before migration. Arbitrary extensions/virtual tables are not sandboxed.
Deterministic SQL across replicas, SQLite durability configuration, schema/metadata
migration and external modification, result/CPU quotas, authentication, retry
deduplication and independent review remain open production gates.
