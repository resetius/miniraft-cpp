# MiniRaft-CPP

This fork's first milestone fixes strict-majority commit selection and FIFO
request completion, and adds deterministic consensus failure tests.
See the [correctness baseline and review](docs/correctness-baseline.md) for
validation, build instructions and remaining production release gates.
The milestone is experimental; durable crash recovery is not established.
The [persistence recovery follow-up](docs/persistence-recovery.md) rejects
incomplete or structurally corrupt storage and fixes truncation/restart behavior.
For new Linux stores, the opt-in [durable state backend](docs/durable-state.md)
adds checksummed atomic publication with file and directory synchronization.
The KV example selects it with `--durable`; legacy `--persist` is unchanged.
The [strong-read follow-up](docs/strong-reads.md) requires a fresh majority for
`--consistent` reads and rejects pending reads when their leadership term ends.
The [pending request follow-up](docs/pending-requests.md) prevents replacement
write acknowledgements and cleans up obsolete or disconnected client requests.
The [forwarding follow-up](docs/forwarding.md) preserves client cookies and binds
forwarded replies to their original leader, term and outbound peer.
The [network framing follow-up](docs/network-frames.md) handles fragmented input
and enforces type, frame, batch and AppendEntries payload bounds.
The [consensus validation follow-up](docs/consensus-validation.md) checks RPC
identities and indices, protects committed prefixes and correlates acknowledgements.
The [application payload follow-up](docs/application-payloads.md) validates KV,
SQL and dummy-RSM data before access and provides pre-admission validation hooks.
The [SQL read policy](docs/sql-readonly.md) prevents requests marked as reads
from executing unreplicated mutations or connection controls.
The [SQL application follow-up](docs/sql-application.md) protects managed write
transactions and commits application progress together with successful changes.
The [SQL read budget follow-up](docs/sql-read-budgets.md) interrupts expensive
reads and bounds serialized output before appending each field or separator.
The [application recovery follow-up](docs/application-recovery.md) preserves
recovered progress and prevents committed commands from being applied again.
The [legacy log-term follow-up](docs/legacy-log-terms.md) rejects impossible
stored term sequences before they influence elections or log matching.
The [snapshot foundation](docs/snapshots.md) adds opt-in durable KV checkpoints
and tail-only application recovery with `--durable --snapshots N`. Logs remain
fully retained in that baseline. The [compaction follow-up](docs/compaction.md)
enables prefix deletion and chunked InstallSnapshot catch-up; fixed membership
remains required. Upgrade all voters before enabling the new compaction behavior.
The [learner follow-up](docs/learners.md) adds non-voting catch-up and durable
voter binding. KV uses `--learner id` on every replica's configuration. Promotion
and joint-consensus voter changes remain disabled.
The [snapshot retry follow-up](docs/snapshot-retries.md) prevents timeout retries
and strong-read traffic from starving delayed snapshot acknowledgements, and
prevents stale reader failures from destroying newer reconnect attempts.
The [snapshot pinning follow-up](docs/snapshot-pinning.md) lets an active transfer
finish while the leader continues compacting newer application state, and avoids
the dependency's unsafe connect-deadline path. TCP connect failure timing follows
the OS; a safe bounded application deadline remains open.
The [proposal admission follow-up](docs/proposal-admission.md) restricts direct
Append calls to leaders, preventing nonleaders from creating conflicting commands
at the same log index and term.
The [memory log ownership follow-up](docs/memory-log-ownership.md) prevents
caller-owned entry buffers from changing entries already stored in TState.
The [term lookup follow-up](docs/term-lookups.md) avoids copying full payloads
when Raft needs only a retained entry's term.
The [joint quorum foundation](docs/joint-quorum.md) adds and tests dual-majority
arithmetic for future membership changes; live voter reconfiguration remains disabled.
The [joint snapshot metadata foundation](docs/joint-snapshots.md) adds a versioned
representation for a joint configuration at a snapshot boundary; runtime adoption
and membership changes remain disabled.
The [snapshot term preflight follow-up](docs/unsupported-snapshot-term-preflight.md)
rejects unsupported snapshot formats before a higher RPC term changes hard state.
The [snapshot metadata preflight follow-up](docs/snapshot-metadata-term-preflight.md)
checks the boundary, fixed voters, and stable identity before that term update.
The [configuration entry foundation](docs/configuration-entries.md) defines an
experimental entry codec and rejects that entry kind in the fixed-membership runtime.
The [configuration history validator](docs/configuration-history.md) checks an
offline retained-log sequence against a trusted baseline or joint snapshot.
The [configuration log scanner](docs/configuration-scan.md) validates a retained
suffix at a caller-supplied commit boundary and at its end.
The [configuration state comparison follow-up](docs/configuration-scan-identity.md)
includes phase and voter sets when reporting an uncommitted configuration.
The [online learner staging API](docs/online-learner-staging.md) lets embedders
add and remove leader-local, non-voting replication targets.
The [network learner follow-up](docs/online-learner-network.md) connects that
API to the built-in server's runtime drain loop.
The [covered-snapshot follow-up](docs/covered-snapshot-identity.md) validates
snapshot boundary and voter metadata before acknowledging an already-covered image.
The [snapshot transfer expiry follow-up](docs/snapshot-transfer-expiry.md)
releases incomplete incoming images after 30 seconds without a valid chunk,
including on learners.
The [snapshot pin budget follow-up](docs/snapshot-pin-budget.md) bounds outgoing
transfer retention to two image generations and defers local compaction at the cap.
The [stable configuration snapshot foundation](docs/stable-configuration-snapshots.md)
preserves configuration identity. Raft can recover and install stable metadata
snapshots when their voter list matches the node's existing fixed membership;
joint snapshots and live voter changes remain disabled.
The [durable configuration snapshot follow-up](docs/durable-configuration-snapshots.md)
documents its voter-binding requirement and recovery boundary.
The [trusted legacy voter binding follow-up](docs/trusted-legacy-voter-binding.md)
adds an explicit migration path when an operator has an independent record of the original set.
The [migration lifecycle follow-up](docs/trusted-legacy-voter-lifecycle.md)
enforces that migration happens before Raft attaches to an unbound legacy store.
The [legacy voter startup proof follow-up](docs/legacy-voter-proof-startup.md)
requires recovered durable stores to provide voter proof before Raft can start.
The [stable snapshot runtime follow-up](docs/stable-configuration-snapshot-runtime.md)
carries that identity through restore, InstallSnapshot, and later compaction.
The [metadata continuity follow-up](docs/stable-snapshot-metadata-continuity.md)
preserves the known identity across legacy-format refreshes and rejects conflicts.
The [snapshot term validation fix](docs/snapshot-committed-terms.md) rejects
snapshot boundaries that contradict the follower's committed prefix.
The [message buffer follow-up](docs/message-extents.md) checks allocated buffer
and batch extents before accepting locally constructed frames.
The [SQL snapshot follow-up](docs/sql-snapshots.md) adds bounded SQLite snapshot
capture, validated restore and recovery from an application checkpoint left
behind a durably installed Raft snapshot.

[![Number of GitHub stars](https://img.shields.io/github/stars/resetius/miniraft-cpp)](https://github.com/resetius/miniraft-cpp/stargazers)
![GitHub commit activity](https://img.shields.io/github/commit-activity/m/resetius/miniraft-cpp)
[![GitHub license which is BSD-2-Clause license](https://img.shields.io/github/license/resetius/miniraft-cpp)](https://github.com/resetius/miniraft-cpp)

## Table of Contents
- [Overview](#overview)
- [Key Features](#key-features)
- [Project Structure](#project-structure)
- [Getting Started](#getting-started)
  - [Prerequisites](#prerequisites)
  - [Building the Project](#building-the-project)
  - [Running the Application](#running-the-application)
- [Example: State Machine (RSM) with Key-Value Store](#example-state-machine-rsm-with-key-value-store)
  - [Server Mode](#server-mode)
  - [Client Mode](#client-mode)
- [Media](#media)

---

## Overview
**MiniRaft-CPP** is a C++20 implementation of the Raft consensus algorithm.
It uses the [coroio library](https://github.com/resetius/coroio) for efficient asynchronous I/O and
aims to provide a straightforward reference for how Raft handles leader election,
log replication, and fault tolerance in a distributed system.

---

## Key Features
- **Leader Election** (supported)
- **Log Replication** (supported)
- **Persistence** (supported)
- **Snapshots** (opt-in KV compaction and InstallSnapshot; Linux durable backend)
- **Membership Change** (non-voting learners; voting changes not supported yet)

The project focuses on a clear, modular design that leverages C++20 coroutines to simplify asynchronous flows.

---

## Project Structure

- **`miniraft/`**
  This directory contains the **MiniRaft library**, which implements all core components required for Raft consensus.
  - **`raft.h` / `raft.cpp`**: Core files defining the Raft algorithm (leader election, log replication, etc.).

- **`miniraft/net/`**
  A **network library** that builds on top of MiniRaft and uses [coroio](https://github.com/resetius/coroio) for asynchronous I/O.
  - **`server.h` / `server.cpp`**: Main networking/server implementation for node-to-node communication and client handling.

- **`examples/`**
  Contains sample applications that demonstrate how to use the MiniRaft libraries:
  - **`kv.cpp`**: A distributed key-value store implemented on top of Raft.


### Using MiniRaft as a Git Submodule
If you want to embed this project into your own codebase as a submodule, you can do one of the following:

1. **Consensus Only**: If you only need the Raft consensus logic without networking, you can include:
```cmake
   add_subdirectory(miniraft)
```

Then link against the miniraft library.

2. **Consensus + Networking**: If you also need the built-in server/network layer, add:

```cmake
add_subdirectory(miniraft)
add_subdirectory(miniraft/net)
```

Then link against miniraft.net which also brings in miniraft internally.

---

## Getting Started

### Prerequisites
- A C++20-compatible compiler
- CMake (for building)
- [Cmocka](https://cmocka.org/) (for unit tests only)

### Building the Project
1. Clone the repository:
   ```
   git clone https://github.com/resetius/miniraft-cpp
   ```
2. Initialize and update the submodule:
   ```
   git submodule init
   git submodule update
   ```
3. Navigate to the project directory:
   ```
   cd miniraft-cpp
   ```
4. Build the project using CMake:
   ```
   cmake .
   make
   ```

### Running the Application

This is a simple application designed to demonstrate log replication in the Raft consensus algorithm.

To start the application, launch the servers with the following commands:
```
./server --id 1 --node 127.0.0.1:8001:1 --node 127.0.0.1:8002:2 --node 127.0.0.1:8003:3
./server --id 2 --node 127.0.0.1:8001:1 --node 127.0.0.1:8002:2 --node 127.0.0.1:8003:3
./server --id 3 --node 127.0.0.1:8001:1 --node 127.0.0.1:8002:2 --node 127.0.0.1:8003:3
```

To interact with the system, run the client as follows:
```
./client --node 127.0.0.1:8001:1
```
The client expects an input string to be added to the distributed log. If the input string starts with an underscore (`_`), it should be followed by a number (e.g., `_ 3`). In this case, the client will attempt to read the log entry at the specified number.

### Example: State Machine (RSM) with Key-Value Store

MiniRaft-CPP includes an example of a distributed Key-Value store implemented as a replicated state machine on top of Raft. The source code for this example resides in examples/kv.cpp

#### Server Mode

To start the KV store servers, use:
```
./kv --server --id 1 --node 127.0.0.1:8001:1 --node 127.0.0.1:8002:2 --node 127.0.0.1:8003:3
./kv --server --id 2 --node 127.0.0.1:8001:1 --node 127.0.0.1:8002:2 --node 127.0.0.1:8003:3
./kv --server --id 3 --node 127.0.0.1:8001:1 --node 127.0.0.1:8002:2 --node 127.0.0.1:8003:3
```

#### Client Mode

To run the KV client, use:
```
./kv --client --node 127.0.0.1:8001:1
```
The KV client expects commands as input:
1. `set <key> <value>` - Adds or updates a value in the KV store.
2. `get <key>` - Retrieves a value by its key.
3. `list` - Displays all key/value pairs in the store.
4. `del <key>` - Deletes a key from the store.

---

## Media
1. [Implementation of the Raft Consensus Algorithm Using C++20 Coroutines](https://dzone.com/articles/implementation-of-the-raft-consensus-algorithm-usi)
2. [Разработка сетевой библиотеки на C++20: интеграция асинхронности и алгоритма Raft (часть 1)](https://www.linux.org.ru/articles/development/17447126)
3. [Разработка сетевой библиотеки на C++20: интеграция асинхронности и алгоритма Raft (часть 2)](https://www.linux.org.ru/articles/development/17449693)
4. [High-performance network library using C++20 coroutines](https://habr.com/ru/articles/768418/)
5. [Simplifying Raft with C++20 coroutines](https://youtu.be/xztv-zIDLxc?si=3amGsi0Co9ZP6flT)
