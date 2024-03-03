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
