cmake_minimum_required(VERSION 3.14)

# TAE_SCANNER_DIR is set by the parent CMakeLists.txt when building from the
# DuckDB extension tree.  Fall back to CMAKE_SOURCE_DIR for standalone builds.
if(NOT DEFINED TAE_SCANNER_DIR)
    set(TAE_SCANNER_DIR ${CMAKE_SOURCE_DIR})
endif()

# Catch2 is bundled inside DuckDB's third_party
include_directories(${CMAKE_SOURCE_DIR}/third_party/catch)

# Test sources
add_executable(tae_tests
    test_main.cpp
    test_types.cpp
    test_zonemap.cpp
    test_reader.cpp
    test_scan_basic.cpp
    test_scan_types.cpp
    test_scan_filters.cpp
    test_scan_errors.cpp
)

target_include_directories(tae_tests PRIVATE
    ${TAE_SCANNER_DIR}/include
    ${DUCKDB_INCLUDE_DIRS}
    ${LZ4_INCLUDE_DIRS}
)

target_link_libraries(tae_tests PRIVATE
    duckdb_static
    dummy_static_extension_loader
    core_functions_extension
    parquet_extension
    jemalloc_extension
    ${LZ4_LIBRARIES}
)

# Link our extension's object files (reader + scanner logic)
# We compile them as an OBJECT library so tests can access internal symbols.
target_sources(tae_tests PRIVATE
    ${TAE_SCANNER_DIR}/src/tae_column_fill.cpp
    ${TAE_SCANNER_DIR}/src/tae_filter.cpp
    ${TAE_SCANNER_DIR}/src/tae_object_reader.cpp
    ${TAE_SCANNER_DIR}/src/tae_scanner.cpp
)

# Use C++17 for std::filesystem and std::make_unique
set_target_properties(tae_tests PROPERTIES CXX_STANDARD 17)

# DuckDB LogicalType static members produce weak symbols in both duckdb_static
# and our compiled sources; allow the linker to merge them.
target_link_options(tae_tests PRIVATE "LINKER:--allow-multiple-definition")

# Point TEST_DATA_DIR to the generated test fixtures
target_compile_definitions(tae_tests PRIVATE
    TEST_DATA_DIR="${TAE_SCANNER_DIR}/test/data"
)

enable_testing()
add_test(NAME tae_tests COMMAND tae_tests)
