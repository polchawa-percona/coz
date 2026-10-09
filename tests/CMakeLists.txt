add_executable(path_filter_test
  ${PROJECT_SOURCE_DIR}/tests/path_filter/path_filter_test.cpp)
target_include_directories(path_filter_test PRIVATE
  ${PROJECT_SOURCE_DIR}/libcoz)
target_compile_features(path_filter_test PRIVATE cxx_std_11)

add_test(NAME path_filter
  COMMAND path_filter_test)

add_executable(dwarf_scope_test
  ${PROJECT_SOURCE_DIR}/tests/dwarf/dwarf_scope_test.cpp)
target_include_directories(dwarf_scope_test PRIVATE
  ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dwarf_scope_test PRIVATE Threads::Threads)
target_compile_features(dwarf_scope_test PRIVATE cxx_std_11)
if(APPLE)
  # macOS toolchains already default to DWARF-4; forcing DWARF-5 causes
  # dsymutil to emit forms libelfin can't parse yet.
  target_compile_options(dwarf_scope_test PRIVATE -g -O2)
else()
  target_compile_options(dwarf_scope_test PRIVATE -g -O2 -gdwarf-5)
endif()

set(TEST_OUTPUT_DIR "${PROJECT_BINARY_DIR}/tests/dwarf_scope")

add_test(NAME dwarf_scope_filter
  COMMAND ${PROJECT_SOURCE_DIR}/tests/run_dwarf_scope_test.sh
          $<TARGET_FILE:coz>
          $<TARGET_FILE:dwarf_scope_test>
          ${TEST_OUTPUT_DIR})
set_tests_properties(dwarf_scope_filter PROPERTIES
  ENVIRONMENT "PYTHONUNBUFFERED=1")

set(RUST_FILTER_OUTPUT_DIR "${PROJECT_BINARY_DIR}/tests/rust_filter")

add_test(NAME rust_source_filter
  COMMAND ${PROJECT_SOURCE_DIR}/tests/run_rust_filter_test.sh
          $<TARGET_FILE:coz>
          ${RUST_FILTER_OUTPUT_DIR})
set_tests_properties(rust_source_filter PROPERTIES
  ENVIRONMENT "PYTHONUNBUFFERED=1")

add_executable(control_test
  ${PROJECT_SOURCE_DIR}/tests/control/control_test.cpp
  ${PROJECT_SOURCE_DIR}/libcoz/control.cpp)
target_include_directories(control_test PRIVATE
  ${PROJECT_SOURCE_DIR}/libcoz)
target_link_libraries(control_test PRIVATE Threads::Threads)
target_compile_features(control_test PRIVATE cxx_std_11)

add_test(NAME control
  COMMAND control_test)
