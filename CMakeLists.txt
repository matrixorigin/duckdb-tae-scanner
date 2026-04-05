cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME tae_scanner)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

include_directories(include)

set(EXTENSION_SOURCES
    src/tae_column_fill.cpp
    src/tae_filter.cpp
    src/tae_object_reader.cpp
    src/tae_scanner.cpp
    src/tae_scanner_extension.cpp
)

# ---------------------------------------------------------------------------
# LZ4 dependency
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
# Build extension using DuckDB's extension build system
# ---------------------------------------------------------------------------
build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Use C++17 for std::filesystem (DuckDB defaults to C++11)
set_target_properties(${EXTENSION_NAME} PROPERTIES CXX_STANDARD 17)
set_target_properties(${LOADABLE_EXTENSION_NAME} PROPERTIES CXX_STANDARD 17)

# Link LZ4 and stdc++fs to both static and loadable targets
target_link_libraries(${EXTENSION_NAME} ${LZ4_LIBRARIES} stdc++fs)
target_link_libraries(${LOADABLE_EXTENSION_NAME} ${LZ4_LIBRARIES} stdc++fs)

if(LZ4_INCLUDE_DIRS)
    target_include_directories(${EXTENSION_NAME} PRIVATE ${LZ4_INCLUDE_DIRS})
    target_include_directories(${LOADABLE_EXTENSION_NAME} PRIVATE ${LZ4_INCLUDE_DIRS})
endif()

install(
    TARGETS ${EXTENSION_NAME}
    EXPORT "${DUCKDB_EXPORT_SET}"
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    ARCHIVE DESTINATION "${INSTALL_LIB_DIR}"
)
