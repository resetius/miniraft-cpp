# MiniRaft-CPP

## Overview
MiniRaft-CPP is an implementation of the Raft consensus algorithm using C++20. This project leverages the [coroio library](https://github.com/resetius/coroio) for efficient asynchronous I/O operations. It aims to provide a clear and efficient representation of the Raft protocol, ensuring consistency and reliability in distributed systems.


## Key Features
- **Leader Election**: Manages the election process for choosing a new leader in the cluster.
- **Log Replication**: Consistently replicates logs across all nodes in the cluster.
- **Safety**: Guarantees the integrity and durability of committed entries.

## Components
- `raft.h` / `raft.cpp`: Implementation of the core Raft algorithm.
- `messages.h` / `messages.cpp`: Message definitions for node communication.
- `timesource.h`: Time-related functionalities for Raft algorithm timings.
- `server.h` / `server.cpp`: Server-side logic for handling client requests and node communication.
- `client.cpp`: Client-side implementation for cluster interaction.

## Getting Started

### Prerequisites
- C++20 compatible compiler
- CMake for building the project
- [Cmocka](https://cmocka.org/) for unit testing

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


### Distributed Key-Value Store Example

Additionally, there's an example implementing a distributed key-value (KV) store. 

#### Starting KV Store Servers

To start the KV store servers, use:
```
./kv --server --id 1 --node 127.0.0.1:8001:1 --node 127.0.0.1:8002:2 --node 127.0.0.1:8003:3
./kv --server --id 2 --node 127.0.0.1:8001:1 --node 127.0.0.1:8002:2 --node 127.0.0.1:8003:3
./kv --server --id 3 --node 127.0.0.1:8001:1 --node 127.0.0.1:8002:2 --node 127.0.0.1:8003:3
```

#### Running the KV Client

To run the KV client, use:
```
./kv --client --node 127.0.0.1:8001:1
```
The KV client expects commands as input:
1. `set <key> <value>` - Adds or updates a value in the KV store.
2. `get <key>` - Retrieves a value by its key.
3. `list` - Displays all key/value pairs in the store.
4. `del <key>` - Deletes a key from the store.

## Media
1. [Implementation of the Raft Consensus Algorithm Using C++20 Coroutines](https://dzone.com/articles/implementation-of-the-raft-consensus-algorithm-usi)
2. [Разработка сетевой библиотеки на C++20: интеграция асинхронности и алгоритма Raft (часть 1)](https://www.linux.org.ru/articles/development/17447126)
3. [Разработка сетевой библиотеки на C++20: интеграция асинхронности и алгоритма Raft (часть 2)](https://www.linux.org.ru/articles/development/17449693)
4. [High-performance network library using C++20 coroutines](https://habr.com/ru/articles/768418/)
5. [Simplifying Raft with C++20 coroutines](https://youtu.be/xztv-zIDLxc?si=3amGsi0Co9ZP6flT)
