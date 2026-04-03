// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0
//
// Tests for TAEObjectReader: reading generated .tae test fixtures.

#include "catch.hpp"
#include "tae_object_reader.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/local_file_system.hpp"

using namespace tae;

// Use compile-time TEST_DATA_DIR from CMake
static std::string DataPath(const char *name) {
    return std::string(TEST_DATA_DIR) + "/" + name;
}

// ===================================================================
// Metadata parsing
// ===================================================================

TEST_CASE("Reader: basic_3col metadata", "[reader]") {
    duckdb::LocalFileSystem fs;
    TAEObjectReader reader(fs, DataPath("basic_3col.tae"));
    reader.ReadMeta();

    auto &meta = reader.Meta();
    REQUIRE(meta.block_count == 1);
    REQUIRE(meta.blocks.size() == 1);

    auto &blk = meta.blocks[0];
    REQUIRE(blk.rows == 8);
    REQUIRE(blk.col_count >= 3); // at least 3 user columns
}

TEST_CASE("Reader: multi_block metadata", "[reader]") {
    duckdb::LocalFileSystem fs;
    TAEObjectReader reader(fs, DataPath("multi_block.tae"));
    reader.ReadMeta();

    REQUIRE(reader.BlockCount() == 2);
    REQUIRE(reader.Meta().blocks[0].rows == 4);
    REQUIRE(reader.Meta().blocks[1].rows == 4);
}

TEST_CASE("Reader: with_nulls metadata", "[reader]") {
    duckdb::LocalFileSystem fs;
    TAEObjectReader reader(fs, DataPath("with_nulls.tae"));
    reader.ReadMeta();

    REQUIRE(reader.BlockCount() == 1);
    REQUIRE(reader.Meta().blocks[0].rows == 6);
}

// ===================================================================
// Block reading: fixed-width columns
// ===================================================================

TEST_CASE("Reader: basic_3col read int32 column", "[reader]") {
    duckdb::LocalFileSystem fs;
    TAEObjectReader reader(fs, DataPath("basic_3col.tae"));
    reader.ReadMeta();

    // Read column 0 (int32) from block 0
    auto cols = reader.ReadBlock(0, {0});
    REQUIRE(cols.size() == 1);

    auto &col = cols[0];
    REQUIRE(col.row_count == 8);
    REQUIRE(col.type.oid == MO_T_int32);
    REQUIRE(col.null_count == 0);

    auto *vals = reinterpret_cast<const int32_t *>(col.data.data());
    // gen_test_data.py writes: 10,20,30,40,50,60,70,80
    for (int i = 0; i < 8; i++) {
        REQUIRE(vals[i] == (i + 1) * 10);
    }
}

TEST_CASE("Reader: basic_3col read float64 column", "[reader]") {
    duckdb::LocalFileSystem fs;
    TAEObjectReader reader(fs, DataPath("basic_3col.tae"));
    reader.ReadMeta();

    // Read column 2 (float64) from block 0
    auto cols = reader.ReadBlock(0, {2});
    REQUIRE(cols.size() == 1);

    auto &col = cols[0];
    REQUIRE(col.type.oid == MO_T_float64);
    REQUIRE(col.row_count == 8);

    auto *vals = reinterpret_cast<const double *>(col.data.data());
    // gen_test_data.py writes: 1.1, 2.2, 3.3, 4.4, 5.5, 6.6, 7.7, 8.8
    REQUIRE(vals[0] == Approx(1.1));
    REQUIRE(vals[7] == Approx(8.8));
}

// ===================================================================
// Block reading: varchar column
// ===================================================================

TEST_CASE("Reader: basic_3col read varchar column", "[reader]") {
    duckdb::LocalFileSystem fs;
    TAEObjectReader reader(fs, DataPath("basic_3col.tae"));
    reader.ReadMeta();

    // Read column 1 (varchar) from block 0
    auto cols = reader.ReadBlock(0, {1});
    REQUIRE(cols.size() == 1);

    auto &col = cols[0];
    REQUIRE(col.type.oid == MO_T_varchar);
    REQUIRE(col.row_count == 8);

    // Varlena: each row is a 24-byte slot
    REQUIRE(col.data.size() == 8 * VARLENA_SIZE);
    auto *slots = reinterpret_cast<const Varlena *>(col.data.data());

    // First string should be "alpha"
    REQUIRE(slots[0].is_inline());
    REQUIRE(slots[0].inline_length() == 5);
    REQUIRE(memcmp(slots[0].inline_data(), "alpha", 5) == 0);
}

// ===================================================================
// Block reading: null bitmap
// ===================================================================

TEST_CASE("Reader: with_nulls has correct null bitmap", "[reader]") {
    duckdb::LocalFileSystem fs;
    TAEObjectReader reader(fs, DataPath("with_nulls.tae"));
    reader.ReadMeta();

    // Read column 0 (int32 with nulls) from block 0
    auto cols = reader.ReadBlock(0, {0});
    REQUIRE(cols.size() == 1);

    auto &col = cols[0];
    REQUIRE(col.row_count == 6);
    // gen_test_data.py: rows 1,3 are null (0-indexed)
    REQUIRE(col.null_count == 2);
    REQUIRE(col.null_bitmap.size() > 0);

    // Check null bitmap bits: bit set = null
    // Row 0: not null, Row 1: null, Row 2: not null, Row 3: null, Row 4: not null, Row 5: not null
    uint64_t bm = col.null_bitmap[0];
    REQUIRE((bm & (1ULL << 0)) == 0);  // row 0: not null
    REQUIRE((bm & (1ULL << 1)) != 0);  // row 1: null
    REQUIRE((bm & (1ULL << 2)) == 0);  // row 2: not null
    REQUIRE((bm & (1ULL << 3)) != 0);  // row 3: null
    REQUIRE((bm & (1ULL << 4)) == 0);  // row 4: not null
    REQUIRE((bm & (1ULL << 5)) == 0);  // row 5: not null
}

// ===================================================================
// Multiple columns in one ReadBlock call
// ===================================================================

TEST_CASE("Reader: read multiple columns at once", "[reader]") {
    duckdb::LocalFileSystem fs;
    TAEObjectReader reader(fs, DataPath("basic_3col.tae"));
    reader.ReadMeta();

    auto cols = reader.ReadBlock(0, {0, 1, 2});
    REQUIRE(cols.size() == 3);
    REQUIRE(cols[0].type.oid == MO_T_int32);
    REQUIRE(cols[1].type.oid == MO_T_varchar);
    REQUIRE(cols[2].type.oid == MO_T_float64);
}

// ===================================================================
// Zone map access
// ===================================================================

TEST_CASE("Reader: zone map access", "[reader]") {
    duckdb::LocalFileSystem fs;
    TAEObjectReader reader(fs, DataPath("basic_3col.tae"));
    reader.ReadMeta();

    const uint8_t *zm = reader.GetZoneMap(0, 0);
    // Zone map should exist for int32 column
    if (zm) {
        ZoneMap z(zm);
        // The int32 column has values 1..8
        // Zone map min should be 1, max should be 8
        REQUIRE(z.GetMin<int32_t>() == 10);
        REQUIRE(z.GetMax<int32_t>() == 80);
    }
}
