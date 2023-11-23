cmake_minimum_required(VERSION 3.10)
project(miniraft)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

set(CMAKE_CXX_STANDARD 20)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CMOCKA REQUIRED cmocka)

add_subdirectory(coroio)

add_library(miniraft
    src/messages.cpp
    src/raft.cpp
)

add_executable(test_raft test/test_raft.cpp)

target_include_directories(test_raft PRIVATE ${CMOCKA_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/coroio/src)
target_link_directories(test_raft PRIVATE ${CMOCKA_LIBRARY_DIRS})
target_link_libraries(test_raft miniraft ${CMOCKA_LIBRARIES})

add_test(NAME test_raft COMMAND ${CMAKE_BINARY_DIR}/test_raft)
