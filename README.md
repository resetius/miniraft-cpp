# MiniRaft-CPP

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
- **Snapshots** (not supported yet)
- **Membership Change** (not supported yet)

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
