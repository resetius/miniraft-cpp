find_package(SQLite3)

macro(target name source)
  add_executable(${name} ${source})
  if (SQLite3_FOUND)
    target_include_directories(${name} PRIVATE ${SQLite3_INCLUDE_DIRS})
    target_link_libraries(${name} PRIVATE miniraft coroio ${SQLite3_LIBRARIES})
  else()
    target_link_libraries(${name} miniraft coroio)
    endif()
endmacro()

target(debug_client client.cpp)
target(debug_server server.cpp)
target(kv kv.cpp)

if (SQLite3_FOUND)
target(sql sql.cpp)
endif()