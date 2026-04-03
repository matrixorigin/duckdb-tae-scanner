// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0
//
// Tests for MO type system: MOType → DuckDB LogicalType mapping,
// fixed-size lookup, and Varlena struct accessors.

#include "catch.hpp"
#include "tae_types.hpp"

using namespace tae;
using namespace duckdb;

// ---------------------------------------------------------------------------
// MOTypeToDuckDB: verify each MO type maps to the correct DuckDB type
// ---------------------------------------------------------------------------

TEST_CASE("MOTypeToDuckDB maps integer types", "[types]") {
    MOType t{};

    t.oid = MO_T_bool;
    REQUIRE(MOTypeToDuckDB(t) == LogicalType::BOOLEAN);

    t.oid = MO_T_int8;
    REQUIRE(MOTypeToDuckDB(t) == LogicalType::TINYINT);

    t.oid = MO_T_int16;
    REQUIRE(MOTypeToDuckDB(t) == LogicalType::SMALLINT);

    t.oid = MO_T_int32;
    REQUIRE(MOTypeToDuckDB(t) == LogicalType::INTEGER);

    t.oid = MO_T_int64;
    REQUIRE(MOTypeToDuckDB(t) == LogicalType::BIGINT);

    t.oid = MO_T_uint8;
    REQUIRE(MOTypeToDuckDB(t) == LogicalType::UTINYINT);

    t.oid = MO_T_uint16;
    REQUIRE(MOTypeToDuckDB(t) == LogicalType::USMALLINT);

    t.oid = MO_T_uint32;
    REQUIRE(MOTypeToDuckDB(t) == LogicalType::UINTEGER);

    t.oid = MO_T_uint64;
    REQUIRE(MOTypeToDuckDB(t) == LogicalType::UBIGINT);
}

TEST_CASE("MOTypeToDuckDB maps float types", "[types]") {
    MOType t{};

    t.oid = MO_T_float32;
    REQUIRE(MOTypeToDuckDB(t) == LogicalType::FLOAT);

    t.oid = MO_T_float64;
    REQUIRE(MOTypeToDuckDB(t) == LogicalType::DOUBLE);
}

TEST_CASE("MOTypeToDuckDB maps decimal types with precision/scale", "[types]") {
    MOType t{};

    t.oid = MO_T_decimal64;
    t.width = 10;
    t.scale = 2;
    auto dt64 = MOTypeToDuckDB(t);
    REQUIRE(dt64.id() == LogicalTypeId::DECIMAL);

    t.oid = MO_T_decimal128;
    t.width = 38;
    t.scale = 10;
    auto dt128 = MOTypeToDuckDB(t);
    REQUIRE(dt128.id() == LogicalTypeId::DECIMAL);
}

TEST_CASE("MOTypeToDuckDB maps string types", "[types]") {
    MOType t{};

    t.oid = MO_T_char;
    REQUIRE(MOTypeToDuckDB(t) == LogicalType::VARCHAR);

    t.oid = MO_T_varchar;
    REQUIRE(MOTypeToDuckDB(t) == LogicalType::VARCHAR);

    t.oid = MO_T_text;
    REQUIRE(MOTypeToDuckDB(t) == LogicalType::VARCHAR);

    t.oid = MO_T_json;
    REQUIRE(MOTypeToDuckDB(t) == LogicalType::VARCHAR);
}

TEST_CASE("MOTypeToDuckDB maps temporal types", "[types]") {
    MOType t{};

    t.oid = MO_T_date;
    REQUIRE(MOTypeToDuckDB(t) == LogicalType::DATE);

    t.oid = MO_T_datetime;
    REQUIRE(MOTypeToDuckDB(t) == LogicalType::TIMESTAMP);

    t.oid = MO_T_timestamp;
    REQUIRE(MOTypeToDuckDB(t) == LogicalType::TIMESTAMP);

    t.oid = MO_T_time;
    REQUIRE(MOTypeToDuckDB(t) == LogicalType::TIME);
}

TEST_CASE("MOTypeToDuckDB maps binary/blob types", "[types]") {
    MOType t{};

    t.oid = MO_T_blob;
    REQUIRE(MOTypeToDuckDB(t) == LogicalType::BLOB);

    t.oid = MO_T_binary;
    REQUIRE(MOTypeToDuckDB(t) == LogicalType::BLOB);

    t.oid = MO_T_varbinary;
    REQUIRE(MOTypeToDuckDB(t) == LogicalType::BLOB);
}

TEST_CASE("MOTypeToDuckDB throws on unknown OID", "[types]") {
    MOType t{};
    t.oid = 255; // nonexistent
    REQUIRE_THROWS_AS(MOTypeToDuckDB(t), std::runtime_error);
}

// ---------------------------------------------------------------------------
// MOTypeFixedSize: verify byte sizes for fixed-width types
// ---------------------------------------------------------------------------

TEST_CASE("MOTypeFixedSize returns correct sizes", "[types]") {
    REQUIRE(MOTypeFixedSize(MO_T_bool) == 1);
    REQUIRE(MOTypeFixedSize(MO_T_int8) == 1);
    REQUIRE(MOTypeFixedSize(MO_T_uint8) == 1);
    REQUIRE(MOTypeFixedSize(MO_T_int16) == 2);
    REQUIRE(MOTypeFixedSize(MO_T_uint16) == 2);
    REQUIRE(MOTypeFixedSize(MO_T_int32) == 4);
    REQUIRE(MOTypeFixedSize(MO_T_uint32) == 4);
    REQUIRE(MOTypeFixedSize(MO_T_float32) == 4);
    REQUIRE(MOTypeFixedSize(MO_T_date) == 4);
    REQUIRE(MOTypeFixedSize(MO_T_int64) == 8);
    REQUIRE(MOTypeFixedSize(MO_T_uint64) == 8);
    REQUIRE(MOTypeFixedSize(MO_T_float64) == 8);
    REQUIRE(MOTypeFixedSize(MO_T_datetime) == 8);
    REQUIRE(MOTypeFixedSize(MO_T_timestamp) == 8);
    REQUIRE(MOTypeFixedSize(MO_T_decimal64) == 8);
    REQUIRE(MOTypeFixedSize(MO_T_decimal128) == 16);
    REQUIRE(MOTypeFixedSize(MO_T_uuid) == 16);
    REQUIRE(MOTypeFixedSize(MO_T_decimal256) == 32);
}

TEST_CASE("MOTypeFixedSize returns -1 for varlena types", "[types]") {
    REQUIRE(MOTypeFixedSize(MO_T_varchar) == -1);
    REQUIRE(MOTypeFixedSize(MO_T_char) == -1);
    REQUIRE(MOTypeFixedSize(MO_T_text) == -1);
    REQUIRE(MOTypeFixedSize(MO_T_blob) == -1);
    REQUIRE(MOTypeFixedSize(MO_T_json) == -1);
}

// ---------------------------------------------------------------------------
// Varlena struct: inline vs big marker
// ---------------------------------------------------------------------------

TEST_CASE("Varlena inline: short string", "[types]") {
    Varlena v{};
    // Inline: length byte + data
    v.data[0] = 5; // length = 5
    memcpy(&v.data[1], "hello", 5);

    REQUIRE(v.is_inline());
    REQUIRE(v.inline_length() == 5);
    REQUIRE(memcmp(v.inline_data(), "hello", 5) == 0);
}

TEST_CASE("Varlena big: offset/length", "[types]") {
    Varlena v{};
    // Big marker
    uint32_t marker = VARLENA_BIG_MARKER;
    memcpy(&v.data[0], &marker, 4);
    uint32_t offset = 100;
    uint32_t length = 200;
    memcpy(&v.data[4], &offset, 4);
    memcpy(&v.data[8], &length, 4);

    REQUIRE_FALSE(v.is_inline());
    REQUIRE(v.big_offset() == 100);
    REQUIRE(v.big_length() == 200);
}
