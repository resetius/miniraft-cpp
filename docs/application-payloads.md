# Application payload validation: v0.1.0-alpha.9

KV key/value lengths and SQL query lengths are checked against the actual
remaining bytes before preparing, reading or applying a command. Checks use the
payload's offset rather than sizeof(derived structure), preserving legacy native
layout/tail padding. Invalid direct application writes do not advance application
progress or mutate KV state.

IRsm gains a default no-op const Validate hook. TRequestProcessor calls it before
queueing, preparing or forwarding requests. KV, SQL and the dummy RSM implement
their shape checks. Custom applications must supply their own validator: the
default preserves source compatibility but cannot infer custom payload formats.
Admission validation exceptions propagate to the inbound handler, closing that
client connection before the invalid operation is admitted.

Read-time standard exceptions are returned as ErrorCode=2 with the original
cookie, allowing other requests and timer processing to continue. Exceptions
while applying a committed entry terminate the process: continuing after a
partial or invalid committed application is unsafe. This is an intentional
behavioral change for embedders whose Write callback throws.

The dummy RSM requires a complete read index and copies only the log entry's
data into its reply, fixing a header-size error that overread by 16 bytes.
Message factories take size_t allocation sizes and reject values above UINT32_MAX
before narrowing/allocation. KV list and SQL reply construction enforce the
existing frame cap. Blank/malformed KV CLI commands no longer dereference missing
tokens or attempt to send an empty request.

KV/SQL state machines are split from their CLI drivers so the real application
methods are directly testable without networking. SQL tests use SQLite's genuine
:memory: database; file-backed naming remains unchanged. SQLite development files
are installed in the core CI job. SQL tests are optional when SQLite is absent.

Fifteen new tests cover malformed lengths, valid KV/SQL round trips, rejection
before admission/forwarding, no mutation/progress on rejection, oversized factory
arguments, dummy read bounds, recoverable read failures and fatal committed
application failures. All 145 tests pass locally on Linux with SQLite under
ASan/UBSan; core Release checks pass. Hosted CI executes the same targets.
The fatal-application subprocess test is excluded on Windows.

Self-review checked validation before data access and admission, widening before
length addition, native-layout offsets, response allocation bounds, CLI tokens
and exception routing. No independent reviewer approval is claimed.

This is payload memory-safety validation, not SQL authorization or complete state
machine correctness. SQL read-only enforcement, nondeterministic SQL, transaction
and metadata semantics, result-set memory quotas, authentication, deadlines,
admission limits and retry deduplication remain open. Callback access remains
serialized and non-reentrant. Write outcomes after interruption can be unknown.
