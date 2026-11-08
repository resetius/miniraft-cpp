# SQL read policy: v0.1.0-alpha.10

The SQL example previously executed read requests through unrestricted
sqlite3_exec. A client could submit UPDATE, schema changes or multiple statements
as a read and mutate one replica without a Raft entry. Six new regression tests
failed against that implementation; the genuine-read positive test already passed.

Reads now prepare exactly one statement under a deny-by-default SQLite authorizer.
The policy permits SELECT, column reads, recursive queries and an explicit list
of trusted built-in scalar/aggregate functions in sql_rsm.cpp. It rejects writes,
DDL, PRAGMA, transaction/savepoint controls, ATTACH/DETACH and unknown functions.
sqlite3_stmt_readonly provides an additional check. The entire trailing input is
prepared and checked before execution, so a second statement is rejected even
if it is another SELECT. Comments, trailing semicolons and semicolons within
strings work. Empty/comment-only input and embedded NUL bytes are rejected.

The policy remains installed through stepping and finalization, including SQLite
automatic recompilation, and RAII removes it on success or failure. Read failures
use the existing ErrorCode=2 response path. Replicated writes retain their existing
execution path and continue working after rejected reads.

SQLite documents why [stmt_readonly alone is insufficient](https://www.sqlite.org/c3ref/stmt_readonly.html)
and how [authorizers apply during preparation and recompilation](https://www.sqlite.org/c3ref/set_authorizer.html).
This policy assumes the example's private connection and trusted SQLite built-ins
and modules. It is not a sandbox for arbitrary registered extensions or virtual
tables. Functions outside the explicit allowlist require a deliberate policy
change; previously accepted read scripts with multiple statements now fail.

Seven tests cover DML/schema rejection, multi-statement rejection before mutation,
connection controls, unsafe functions, real SELECT/CTE behavior, malformed text
and restoration before a subsequent write. All 152 tests pass locally across 11
CTest targets under ASan/UBSan; the Release core suite also passes with SQLite.
Self-review checked policy lifetime, preparation before stepping, function-name
handling, statement cleanup and unchanged replicated-write routing. No independent
reviewer approval is claimed.

SQL write transaction escape, metadata/progress semantics, deterministic execution,
result memory/CPU quotas, authentication and retry deduplication remain open.
The project remains experimental and does not yet provide NuRaft-level assurance.
