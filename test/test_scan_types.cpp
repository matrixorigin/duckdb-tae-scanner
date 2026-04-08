// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0
//
// Extended type tests: DECIMAL64/128, UUID, BLOB, Date, Timestamp.

#include "test_helpers.hpp"

// ===================================================================
// DECIMAL64 type tests
// ===================================================================

TEST_CASE("Scan: decimal64 column returns correctly scaled values", "[types]") {
    auto db = MakeDB();
    auto result = Query(*db, "SELECT col_dec64 FROM tae_scan('" +
                              ManifestPath("manifest_types.json") +
                              "') ORDER BY col_ref");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 6);
    // Values in test data: [12345, 67890, -9999, 0, 100, 99999] scaled by 100
    // So: 123.45, 678.90, -99.99, 0.00, 1.00, 999.99
    CHECK(result->GetValue(0, 0) == Value::DECIMAL(12345, 10, 2));
    CHECK(result->GetValue(0, 1) == Value::DECIMAL(67890, 10, 2));
    CHECK(result->GetValue(0, 2) == Value::DECIMAL(-9999, 10, 2));
    CHECK(result->GetValue(0, 3) == Value::DECIMAL(int64_t(0), 10, 2));
    CHECK(result->GetValue(0, 4) == Value::DECIMAL(int64_t(100), 10, 2));
    CHECK(result->GetValue(0, 5) == Value::DECIMAL(99999, 10, 2));
}

TEST_CASE("Scan: decimal64 filter pushdown works", "[types]") {
    auto db = MakeDB();
    auto result = Query(*db, "SELECT col_dec64, col_ref FROM tae_scan('" +
                              ManifestPath("manifest_types.json") +
                              "') WHERE col_dec64 > 100.00 ORDER BY col_ref");
    REQUIRE_FALSE(result->HasError());
    // 123.45, 678.90, 999.99 pass (> 100.00)
    REQUIRE(result->RowCount() == 3);
    CHECK(result->GetValue(1, 0) == Value::INTEGER(1));  // 123.45
    CHECK(result->GetValue(1, 1) == Value::INTEGER(2));  // 678.90
    CHECK(result->GetValue(1, 2) == Value::INTEGER(6));  // 999.99
}

// ===================================================================
// DECIMAL128 type tests
// ===================================================================

TEST_CASE("Scan: decimal128 column returns correctly scaled values", "[types]") {
    auto db = MakeDB();
    auto result = Query(*db, "SELECT col_dec128 FROM tae_scan('" +
                              ManifestPath("manifest_types.json") +
                              "') ORDER BY col_ref");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 6);
    // Test a couple of values
    CHECK(result->GetValue(0, 0) == Value::DECIMAL(hugeint_t(123456789012345), 20, 4));
    CHECK(result->GetValue(0, 2) == Value::DECIMAL(hugeint_t(0), 20, 4));
}

// ===================================================================
// UUID type tests
// ===================================================================

TEST_CASE("Scan: uuid column returns correct values", "[types]") {
    auto db = MakeDB();
    auto result = Query(*db, "SELECT col_uuid FROM tae_scan('" +
                              ManifestPath("manifest_types.json") +
                              "') ORDER BY col_ref");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 6);
    // Verify UUID strings (DuckDB stores as hugeint, prints as UUID string)
    auto v0 = result->GetValue(0, 0).ToString();
    CHECK(v0 == "00000000-0000-0000-0000-000000000001");
    auto v1 = result->GetValue(0, 1).ToString();
    CHECK(v1 == "11111111-1111-1111-1111-111111111111");
    auto v3 = result->GetValue(0, 3).ToString();
    CHECK(v3 == "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa");
}

TEST_CASE("Scan: uuid filter pushdown works", "[types]") {
    auto db = MakeDB();
    auto result = Query(*db, "SELECT col_uuid, col_ref FROM tae_scan('" +
                              ManifestPath("manifest_types.json") +
                              "') WHERE col_uuid = '11111111-1111-1111-1111-111111111111'");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 1);
    CHECK(result->GetValue(1, 0) == Value::INTEGER(2));
}

// ===================================================================
// BLOB type tests
// ===================================================================

TEST_CASE("Scan: blob column returns correct values", "[types]") {
    auto db = MakeDB();
    auto result = Query(*db, "SELECT col_blob, col_ref FROM tae_scan('" +
                              ManifestPath("manifest_types.json") +
                              "') ORDER BY col_ref");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 6);
    // Row 0: \x00\x01\x02\x03
    auto blob0 = result->GetValue(0, 0);
    CHECK(blob0.type().id() == LogicalTypeId::BLOB);
    auto s0 = StringValue::Get(blob0);
    REQUIRE(s0.size() == 4);
    CHECK(static_cast<uint8_t>(s0[0]) == 0x00);
    CHECK(static_cast<uint8_t>(s0[1]) == 0x01);
    CHECK(static_cast<uint8_t>(s0[3]) == 0x03);
    // Row 1: \xDE\xAD\xBE\xEF
    auto s1 = StringValue::Get(result->GetValue(0, 1));
    REQUIRE(s1.size() == 4);
    CHECK(static_cast<uint8_t>(s1[0]) == 0xDE);
    CHECK(static_cast<uint8_t>(s1[3]) == 0xEF);
    // Row 4: empty blob
    auto s4 = StringValue::Get(result->GetValue(0, 4));
    CHECK(s4.empty());
}

// ===================================================================
// All types together — projection test
// ===================================================================

TEST_CASE("Scan: project subset of extended types", "[types]") {
    auto db = MakeDB();
    auto result = Query(*db, "SELECT col_uuid, col_ref FROM tae_scan('" +
                              ManifestPath("manifest_types.json") +
                              "') ORDER BY col_ref");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 6);
    // Only 2 columns projected
    REQUIRE(result->ColumnCount() == 2);
    CHECK(result->GetValue(0, 5).ToString() == "01234567-89ab-cdef-0123-456789abcdef");
    CHECK(result->GetValue(1, 5) == Value::INTEGER(6));
}

// ===================================================================
// Date / Timestamp types
// ===================================================================

TEST_CASE("Scan: date column returns correct calendar dates", "[types][datetime]") {
    auto db = MakeDB();
    auto result = Query(*db, "SELECT col_date, col_ref FROM tae_scan('" +
                              ManifestPath("manifest_datetime.json") +
                              "') ORDER BY col_ref");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 6);
    // Dates: 2024-01-15, 2000-01-01, 1970-01-01, 1969-12-31, 2024-12-31, 1999-06-15
    CHECK(result->GetValue(0, 0).ToString() == "2024-01-15");
    CHECK(result->GetValue(0, 1).ToString() == "2000-01-01");
    CHECK(result->GetValue(0, 2).ToString() == "1970-01-01");
    CHECK(result->GetValue(0, 3).ToString() == "1969-12-31");
    CHECK(result->GetValue(0, 4).ToString() == "2024-12-31");
    CHECK(result->GetValue(0, 5).ToString() == "1999-06-15");
}

TEST_CASE("Scan: timestamp column returns correct values", "[types][datetime]") {
    auto db = MakeDB();
    auto result = Query(*db, "SELECT col_ts, col_ref FROM tae_scan('" +
                              ManifestPath("manifest_datetime.json") +
                              "') ORDER BY col_ref");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 6);
    // Timestamps: 2024-01-15 12:30:45, 2000-01-01 00:00:00, 1970-01-01 00:00:00,
    //             2024-06-15 23:59:59.999, 1999-12-31 23:59:59, 2020-02-29 00:00:00
    CHECK(result->GetValue(0, 0).ToString() == "2024-01-15 12:30:45");
    CHECK(result->GetValue(0, 1).ToString() == "2000-01-01 00:00:00");
    CHECK(result->GetValue(0, 2).ToString() == "1970-01-01 00:00:00");
    CHECK(result->GetValue(0, 3).ToString() == "2024-06-15 23:59:59.999");
    CHECK(result->GetValue(0, 4).ToString() == "1999-12-31 23:59:59");
    CHECK(result->GetValue(0, 5).ToString() == "2020-02-29 00:00:00");
}

TEST_CASE("Scan: date filter pushdown works", "[types][datetime]") {
    auto db = MakeDB();
    auto result = Query(*db, "SELECT col_date FROM tae_scan('" +
                              ManifestPath("manifest_datetime.json") +
                              "') WHERE col_date >= DATE '2024-01-01' ORDER BY col_date");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 2);
    CHECK(result->GetValue(0, 0).ToString() == "2024-01-15");
    CHECK(result->GetValue(0, 1).ToString() == "2024-12-31");
}

TEST_CASE("Scan: timestamp filter pushdown works", "[types][datetime]") {
    auto db = MakeDB();
    auto result = Query(*db, "SELECT col_ts FROM tae_scan('" +
                              ManifestPath("manifest_datetime.json") +
                              "') WHERE col_ts < TIMESTAMP '2000-01-01 00:00:01' ORDER BY col_ts");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 3);
    CHECK(result->GetValue(0, 0).ToString() == "1970-01-01 00:00:00");
    CHECK(result->GetValue(0, 1).ToString() == "1999-12-31 23:59:59");
    CHECK(result->GetValue(0, 2).ToString() == "2000-01-01 00:00:00");
}

TEST_CASE("Stats: date MIN/MAX aggregates", "[stats][datetime]") {
    auto db = MakeDB();
    auto result = Query(*db, "SELECT MIN(col_date), MAX(col_date) FROM tae_scan('" +
                              ManifestPath("manifest_datetime.json") + "')");
    REQUIRE_FALSE(result->HasError());
    CHECK(result->GetValue(0, 0).ToString() == "1969-12-31");
    CHECK(result->GetValue(1, 0).ToString() == "2024-12-31");
}

TEST_CASE("Stats: timestamp MIN/MAX aggregates", "[stats][datetime]") {
    auto db = MakeDB();
    auto result = Query(*db, "SELECT MIN(col_ts), MAX(col_ts) FROM tae_scan('" +
                              ManifestPath("manifest_datetime.json") + "')");
    REQUIRE_FALSE(result->HasError());
    CHECK(result->GetValue(0, 0).ToString() == "1970-01-01 00:00:00");
    CHECK(result->GetValue(1, 0).ToString() == "2024-06-15 23:59:59.999");
}
