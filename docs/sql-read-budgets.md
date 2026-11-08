# SQL read budgets: v0.1.0-alpha.12

SQL reads previously had no execution budget and collected all rows before
serializing and checking the response frame size. Even a small recursive query
could block the single event loop indefinitely; a large result could exhaust
memory before the final size check. Six regression tests failed with the previous
read implementation (with the new limit configuration accepted but unenforced).

TSql now accepts an optional third constructor argument, TSqlReadLimits. Defaults
apply to the existing SQL server without CLI changes:

* MaxVmSteps defaults to 1,000,000. SQLite progress callbacks interrupt long reads
  during preparation or stepping. This is an approximate instruction budget,
  checked at intervals of at most 1,000 VM instructions, not a wall-clock deadline
  or an exact cross-version instruction counter. Zero is rejected.
* MaxResultBytes defaults to the existing 16 MiB frame cap minus the response
  header. A single serialized buffer is built while stepping; headers, commas,
  NULL markers, data bytes and newlines count toward the limit before each append.
  Partial output is discarded on failure. A smaller nonnegative limit is allowed;
  exceeding the response payload cap is rejected at construction.
* SQLITE_LIMIT_LENGTH is temporarily lowered to 16 MiB for reads, restricting
  individual strings, blobs and encoded rows before application copying. The
  previous connection limit is restored afterward. Oversized stored rows may
  therefore fail reads even when the requested output is small.

Prepared statements finalize before the authorizer, progress callback and length
limit are removed/restored, on both success and exceptions. Writes retain their
existing execution policy and length limit. Rejected reads do not change applied
progress. The existing request processor maps read exceptions to ErrorCode=2 and
preserves client cookies; subsequent requests continue normally.

Removing the intermediate row collection also fixes silent truncation at embedded
NUL bytes: value serialization uses SQLite's byte length after text conversion.
The reply remains the example's simple comma/newline format, including a single
newline for an empty result. It does not add CSV escaping or typed blob encoding.

Seven new tests cover finite expensive aggregate queries with explicit/default
budgets, exact output boundaries (including empty results), accumulated rows,
recovery through ordinary/strong request processing, binary text lengths, invalid
configuration and restoration of SQLite's value limit before writes. Tests use
SQL computation and fixed byte limits; they need no timing assumptions or sleeps.
All 169 tests across 11 CTest targets pass locally with ASan/UBSan and SQLite;
the Release core suite passes as well.

Self-review checked callback state and lifetime, constructor validation, cleanup
order, subtraction before append, NULL versus conversion failure, SQLite text/byte
conversion order and the unchanged write path. No independent reviewer approval
is claimed. Relevant SQLite contracts are [progress callbacks](https://www.sqlite.org/c3ref/progress_handler.html),
[runtime limits](https://www.sqlite.org/c3ref/limit.html), and
[column conversion and byte lengths](https://www.sqlite.org/c3ref/column_blob.html).

This closes unbounded read VM execution and application result accumulation, not
all resource-exhaustion risks. A VM instruction or built-in function can itself
be expensive; temporary sorting/storage, preparation memory, total SQLite heap,
database size and committed-write execution need separate controls. There is no
hard CPU-time or process-memory bound. Authentication, admission/backpressure,
deterministic SQL, SQLite durability policy and independent review remain open.
