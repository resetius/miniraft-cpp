cmake_minimum_required(VERSION 3.10)
project(miniraft)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if (EXISTS ${CMAKE_BINARY_DIR}/local.cmake)
include(${CMAKE_BINARY_DIR}/local.cmake)
endif ()

enable_testing()

set(CMAKE_CXX_STANDARD 20)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CMOCKA REQUIRED cmocka)

add_subdirectory(coroio)

add_library(miniraft
    src/messages.cpp
    src/raft.cpp
    src/server.cpp
)

target_link_libraries(miniraft PUBLIC coroio)

add_executable(test_raft test/test_raft.cpp)
add_executable(test_read_write test/test_read_write.cpp)
add_executable(server server/server.cpp)
add_executable(client client/client.cpp)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/coroio)

target_link_libraries(server miniraft coroio)
target_link_libraries(client miniraft coroio)

target_include_directories(test_raft PRIVATE ${CMOCKA_INCLUDE_DIRS})
target_link_directories(test_raft PRIVATE ${CMOCKA_LIBRARY_DIRS})
target_link_libraries(test_raft miniraft coroio ${CMOCKA_LIBRARIES})

add_test(NAME test_raft COMMAND ${CMAKE_BINARY_DIR}/test_raft)
set_tests_properties(test_raft PROPERTIES ENVIRONMENT "CMOCKA_MESSAGE_OUTPUT=xml;CMOCKA_XML_FILE=test_raft.xml")

target_include_directories(test_read_write PRIVATE ${CMOCKA_INCLUDE_DIRS})
target_link_directories(test_read_write PRIVATE ${CMOCKA_LIBRARY_DIRS})
target_link_libraries(test_read_write miniraft coroio ${CMOCKA_LIBRARIES})

add_test(NAME test_read_write COMMAND ${CMAKE_BINARY_DIR}/test_read_write)
set_tests_properties(test_read_write PROPERTIES ENVIRONMENT "CMOCKA_MESSAGE_OUTPUT=xml;CMOCKA_XML_FILE=test_read_write.xml")
