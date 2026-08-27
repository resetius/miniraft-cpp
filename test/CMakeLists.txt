find_package(PkgConfig REQUIRED)
pkg_check_modules(CMOCKA REQUIRED cmocka)

macro(ut name source lib)
  add_executable(${name} ${source})
  target_include_directories(${name} PRIVATE ${CMOCKA_INCLUDE_DIRS})
  target_link_directories(${name} PRIVATE ${CMOCKA_LIBRARY_DIRS})
  target_link_libraries(${name} PRIVATE ${lib} ${CMOCKA_LIBRARIES})

  add_test(NAME ${name} COMMAND ${CMAKE_CURRENT_BINARY_DIR}/${name})
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "CMOCKA_MESSAGE_OUTPUT=xml;CMOCKA_XML_FILE=${name}.xml")
endmacro()

if (CMOCKA_FOUND)
ut(test_raft test_raft.cpp miniraft)
ut(test_read_write test_read_write.cpp miniraft.net)
endif ()
