# MiniRaft-CPP

## Overview
MiniRaft-CPP is an implementation of the Raft consensus algorithm using C++20. This project aims to provide a clear and efficient representation of the Raft protocol, ensuring consistency and reliability in distributed systems. The implementation includes leader election, log replication, and safety mechanisms as per the Raft specification.

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
- Start the servers: 
  ```
  ./server --id 1 --node 127.0.0.1:8001:1 --node 127.0.0.1:8002:2 --node 127.0.0.1:8003:3
  ./server --id 2 --node 127.0.0.1:8001:1 --node 127.0.0.1:8002:2 --node 127.0.0.1:8003:3
  ./server --id 3 --node 127.0.0.1:8001:1 --node 127.0.0.1:8002:2 --node 127.0.0.1:8003:3
  ```
- Run the client (the client can now only work with the leader): 
  ```
  ./client --node 127.0.0.1:8001:1
  ```
