cmake_minimum_required(VERSION 3.14)

# Catch2 is bundled inside DuckDB's third_party
include_directories(${DUCKDB_DIR}/third_party/catch)

# Test sources
add_executable(tae_tests
    test_main.cpp
    test_types.cpp
    test_zonemap.cpp
    test_reader.cpp
    test_scan.cpp
)

target_include_directories(tae_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${DUCKDB_INCLUDE_DIRS}
    ${LZ4_INCLUDE_DIRS}
)

target_link_libraries(tae_tests PRIVATE
    duckdb_static
    duckdb_generated_extension_loader
    core_functions_extension
    parquet_extension
    jemalloc_extension
    ${LZ4_LIBRARIES}
)

# Link our extension's object files (reader + scanner logic)
# We compile them as an OBJECT library so tests can access internal symbols.
target_sources(tae_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/tae_column_fill.cpp
    ${CMAKE_SOURCE_DIR}/src/tae_filter.cpp
    ${CMAKE_SOURCE_DIR}/src/tae_object_reader.cpp
    ${CMAKE_SOURCE_DIR}/src/tae_scanner.cpp
)

# Point TEST_DATA_DIR to the generated test fixtures
target_compile_definitions(tae_tests PRIVATE
    TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/test/data"
)

enable_testing()
add_test(NAME tae_tests COMMAND tae_tests)
