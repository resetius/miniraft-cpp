cmake_minimum_required(VERSION 3.10)
project(miniraft)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if (EXISTS ${CMAKE_BINARY_DIR}/local.cmake)
include(${CMAKE_BINARY_DIR}/local.cmake)
endif ()

enable_testing()

add_subdirectory(coroio/coroio)
add_subdirectory(miniraft)
add_subdirectory(miniraft/net)
add_subdirectory(examples)
add_subdirectory(test)