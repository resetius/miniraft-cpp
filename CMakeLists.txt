cmake_minimum_required(VERSION 3.10)
project(miniraft)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

set(CMAKE_CXX_STANDARD 20)

add_subdirectory(coroio)

add_library(miniraft
    src/messages.cpp
    src/raft.cpp
)
