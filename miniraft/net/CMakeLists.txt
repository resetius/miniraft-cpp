add_library(miniraft.net
    server.cpp
)

target_link_libraries(miniraft.net PUBLIC coroio miniraft)
target_compile_features(miniraft.net PUBLIC cxx_std_20)
