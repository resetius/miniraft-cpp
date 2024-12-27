cmake_minimum_required(VERSION 3.10)
project(miniraft)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if (EXISTS ${CMAKE_BINARY_DIR}/local.cmake)
include(${CMAKE_BINARY_DIR}/local.cmake)
endif ()

enable_testing()

add_subdirectory(coroio/coroio)
add_subdirectory(src)

add_executable(server server/server.cpp)
add_executable(client client/client.cpp)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(server miniraft coroio)
target_link_libraries(client miniraft coroio)

add_subdirectory(examples)
add_subdirectory(test)