# Correctness baseline: v0.1.0-alpha.1

This fork starts from upstream a2848b843b3910664216a00605a406b5516e0f8c.
This is an experimental correctness milestone, not a production-readiness claim.

## Fixes

* Select the strict-majority replication index for both odd and even cluster
  sizes. For four nodes, two replicas must never commit an entry.
* Process request queues from the front, matching std::queue::pop(). This
  preserves FIFO replies for reads, strong reads and writes, and allows an
  earlier committed write to complete while a later write remains pending.
* Resolve the pinned coroio submodule against its upstream URL so forks clone
  successfully. The pinned dependency commit is unchanged.
* Support MINIRAFT_BUILD_NETWORK=OFF and BUILD_TESTING=OFF independently.

## Deterministic failure harness

test/test_correctness.cpp uses virtual time and an explicit message queue.
It uses no sockets, wall clock, sleeps or nondeterministic scheduling.
The eight tests cover:

1. Every replica count for cluster sizes one through eight.
2. FIFO normal-read replies.
3. FIFO strong-read replies.
4. FIFO write replies.
5. Partial write completion without blocking earlier requests.
6. Dropped replication traffic followed by retry, reversal and duplication.
7. Leader isolation, majority election and commit, delayed old-term messages,
   conflicting minority-log replacement and partition healing.
8. A four-node partition where two replicas cannot commit, followed by healing.

Cluster scenarios check commit progress and equality of committed log prefixes.
These are selected deterministic schedules, not exhaustive state-space exploration.
Restart and storage crash injection are not covered by the simulator yet.

## Reproduce

Install a C++20 compiler, CMake, pkg-config and cmocka development files.
For the full build, initialize submodules and install coroio dependencies
(on Ubuntu: liburing-dev, libssl-dev and libsqlite3-dev).

```sh
cmake -S . -B build/core -DMINIRAFT_BUILD_NETWORK=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build build/core --parallel 4
ctest --test-dir build/core --output-on-failure

git submodule update --init --recursive
cmake -S . -B build/full -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build/full --parallel 4
ctest --test-dir build/full --output-on-failure
```

## Review and validation

The implementing agent performed a source self-review of the final diff,
majority arithmetic, queue ownership and completion ordering, build options,
dependency pin and deterministic test schedules. No independent human or
second-agent review has been obtained. The release is explicitly an alpha.

Local validation: the 27 upstream core tests, eight new correctness tests and
three upstream transport tests pass. The full suite also passes with ASan and
UBSan. The new tests fail against the upstream implementation: majority and
read-order assertions fail, and queued write handling aborts. Local validation
is separate from hosted GitHub Actions results.

## Remaining release gates before production

* Replace stream-only persistence with a defined durable write protocol, ordered
  synchronization, recovery validation and crash-injection testing. Current
  TDiskState is not validated for power-loss durability.
* Audit strong-read quorum freshness, pending operations across leadership
  changes, client retry/deduplication semantics and malformed message handling.
  FIFO tests do not establish linearizability.
* Add deterministic node restart and broader generated fault schedules, checking
  election safety and committed application histories.
* Implement and validate snapshots, compaction and safe membership changes.
* Establish real-network load/failure testing, backpressure and operational APIs.
