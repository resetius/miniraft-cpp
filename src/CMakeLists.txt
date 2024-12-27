add_library(miniraft
    messages.cpp
    raft.cpp
    server.cpp
    persist.cpp
)

target_include_directories(miniraft PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(miniraft PUBLIC coroio)
target_compile_features(miniraft PUBLIC cxx_std_20)