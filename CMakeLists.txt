cmake_minimum_required(VERSION 3.14)
project(tae_scanner VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# DuckDB — expects DUCKDB_DIR to point at a DuckDB source/install tree
# ---------------------------------------------------------------------------
if(NOT DEFINED DUCKDB_DIR)
    message(FATAL_ERROR "Set -DDUCKDB_DIR=<path-to-duckdb> (source or install root)")
endif()

# Try find_package first (installed DuckDB), fallback to source tree
find_package(DuckDB QUIET HINTS ${DUCKDB_DIR})
if(NOT DuckDB_FOUND)
    # Source-tree: add as subdirectory
    if(EXISTS ${DUCKDB_DIR}/CMakeLists.txt)
        add_subdirectory(${DUCKDB_DIR} duckdb_build EXCLUDE_FROM_ALL)
        set(DUCKDB_INCLUDE_DIRS
            ${DUCKDB_DIR}/src/include
            ${DUCKDB_DIR}/third_party/yyjson/include
        )
        set(DUCKDB_LIBRARIES duckdb)
    else()
        # Headers + lib in install prefix
        set(DUCKDB_INCLUDE_DIRS ${DUCKDB_DIR}/include)
        find_library(DUCKDB_LIBRARIES NAMES duckdb HINTS ${DUCKDB_DIR}/lib)
        if(NOT DUCKDB_LIBRARIES)
            message(FATAL_ERROR "Cannot find DuckDB library in ${DUCKDB_DIR}/lib")
        endif()
    endif()
else()
    get_target_property(DUCKDB_INCLUDE_DIRS DuckDB::DuckDB INTERFACE_INCLUDE_DIRECTORIES)
    set(DUCKDB_LIBRARIES DuckDB::DuckDB)
endif()

# ---------------------------------------------------------------------------
# LZ4
# ---------------------------------------------------------------------------
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LZ4 QUIET liblz4)
endif()
if(NOT LZ4_FOUND)
    find_library(LZ4_LIBRARIES NAMES lz4)
    find_path(LZ4_INCLUDE_DIRS NAMES lz4.h)
    if(NOT LZ4_LIBRARIES OR NOT LZ4_INCLUDE_DIRS)
        message(FATAL_ERROR "LZ4 not found. Install liblz4-dev or set LZ4 paths.")
    endif()
endif()

# ---------------------------------------------------------------------------
# Extension shared library
# ---------------------------------------------------------------------------
add_library(tae_scanner SHARED
    src/tae_column_fill.cpp
    src/tae_filter.cpp
    src/tae_object_reader.cpp
    src/tae_scanner.cpp
    src/tae_scanner_extension.cpp
)

target_include_directories(tae_scanner PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${DUCKDB_INCLUDE_DIRS}
    ${LZ4_INCLUDE_DIRS}
)

target_link_libraries(tae_scanner PRIVATE
    ${DUCKDB_LIBRARIES}
    ${LZ4_LIBRARIES}
)

# DuckDB extension naming convention
set_target_properties(tae_scanner PROPERTIES
    PREFIX ""
    OUTPUT_NAME "tae_scanner"
)

# ---------------------------------------------------------------------------
# Tests (opt-in via BUILD_TESTS or when top-level project)
# ---------------------------------------------------------------------------
option(BUILD_TESTS "Build Catch2 unit tests" ON)
if(BUILD_TESTS)
    add_subdirectory(test)
endif()

# Install
install(TARGETS tae_scanner LIBRARY DESTINATION lib)
