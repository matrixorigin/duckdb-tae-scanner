// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0
//
// Filter pushdown, ORDER BY, LZ4, statistics callbacks, partition pruning.

#include "test_helpers.hpp"

// ===================================================================
// LZ4-compressed data
// ===================================================================

TEST_CASE("Scan: LZ4-compressed columns read correctly", "[lz4]") {
    auto db = MakeDB();
    auto result = Query(*db, "SELECT * FROM tae_scan('" +
                              ManifestPath("manifest_lz4.json") +
                              "') ORDER BY col_int");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 8);
    REQUIRE(result->ColumnCount() == 3);

    // Same data as basic_3col: int32 [10..80], varchar, float64
    CHECK(result->GetValue(0, 0) == Value::INTEGER(10));
    CHECK(result->GetValue(0, 7) == Value::INTEGER(80));
    CHECK(result->GetValue(1, 0).ToString() == "alpha");
    CHECK(result->GetValue(1, 7).ToString() == "theta");
    CHECK(result->GetValue(2, 0) == Value::DOUBLE(1.1));
    CHECK(result->GetValue(2, 7) == Value::DOUBLE(8.8));
}

TEST_CASE("Scan: LZ4-compressed with filter pushdown", "[lz4][filter]") {
    auto db = MakeDB();
    auto result = Query(*db, "SELECT col_int, col_str FROM tae_scan('" +
                              ManifestPath("manifest_lz4.json") +
                              "') WHERE col_int > 50 ORDER BY col_int");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 3);  // 60, 70, 80
    CHECK(result->GetValue(0, 0) == Value::INTEGER(60));
    CHECK(result->GetValue(0, 1) == Value::INTEGER(70));
    CHECK(result->GetValue(0, 2) == Value::INTEGER(80));
    CHECK(result->GetValue(1, 0).ToString() == "zeta");
    CHECK(result->GetValue(1, 1).ToString() == "eta");
    CHECK(result->GetValue(1, 2).ToString() == "theta");
}

TEST_CASE("Scan: LZ4-compressed with string filter", "[lz4][filter]") {
    auto db = MakeDB();
    auto result = Query(*db, "SELECT col_int, col_str FROM tae_scan('" +
                              ManifestPath("manifest_lz4.json") +
                              "') WHERE col_str = 'gamma'");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 1);
    CHECK(result->GetValue(0, 0) == Value::INTEGER(30));
    CHECK(result->GetValue(1, 0).ToString() == "gamma");
}

TEST_CASE("Scan: LZ4-compressed projection subset", "[lz4]") {
    auto db = MakeDB();
    auto result = Query(*db, "SELECT col_dbl FROM tae_scan('" +
                              ManifestPath("manifest_lz4.json") +
                              "') ORDER BY col_dbl");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 8);
    REQUIRE(result->ColumnCount() == 1);
    CHECK(result->GetValue(0, 0) == Value::DOUBLE(1.1));
    CHECK(result->GetValue(0, 4) == Value::DOUBLE(5.5));
}

TEST_CASE("Scan: LZ4-compressed EXPLAIN works", "[lz4]") {
    auto db = MakeDB();
    auto result = Query(*db, "EXPLAIN SELECT * FROM tae_scan('" +
                              ManifestPath("manifest_lz4.json") + "')");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() > 0);
}

// ===================================================================
// ORDER BY pushdown (set_scan_order)
// ===================================================================

TEST_CASE("Scan: ORDER BY ASC LIMIT returns correct values", "[orderby]") {
    auto db = MakeDB();
    // multi_block.tae: block 0 = [1,2,3,4], block 1 = [100,200,300,400]
    // ORDER BY col_int ASC LIMIT 3 should return [1, 2, 3]
    auto result = Query(*db, "SELECT col_int FROM tae_scan('" +
                              ManifestPath("manifest_sorted.json") +
                              "') ORDER BY col_int ASC LIMIT 3");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 3);
    CHECK(result->GetValue(0, 0) == Value::INTEGER(1));
    CHECK(result->GetValue(0, 1) == Value::INTEGER(2));
    CHECK(result->GetValue(0, 2) == Value::INTEGER(3));
}

TEST_CASE("Scan: ORDER BY DESC LIMIT returns correct values", "[orderby]") {
    auto db = MakeDB();
    // ORDER BY col_int DESC LIMIT 3 should return [400, 300, 200]
    auto result = Query(*db, "SELECT col_int FROM tae_scan('" +
                              ManifestPath("manifest_sorted.json") +
                              "') ORDER BY col_int DESC LIMIT 3");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 3);
    CHECK(result->GetValue(0, 0) == Value::INTEGER(400));
    CHECK(result->GetValue(0, 1) == Value::INTEGER(300));
    CHECK(result->GetValue(0, 2) == Value::INTEGER(200));
}

TEST_CASE("Scan: ORDER BY LIMIT larger than one block", "[orderby]") {
    auto db = MakeDB();
    // LIMIT 6 needs both blocks (4 + 4 rows)
    auto result = Query(*db, "SELECT col_int FROM tae_scan('" +
                              ManifestPath("manifest_sorted.json") +
                              "') ORDER BY col_int ASC LIMIT 6");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 6);
    CHECK(result->GetValue(0, 0) == Value::INTEGER(1));
    CHECK(result->GetValue(0, 5) == Value::INTEGER(200));
}

TEST_CASE("Scan: ORDER BY without LIMIT still works", "[orderby]") {
    auto db = MakeDB();
    // Without LIMIT, set_scan_order won't fire but query should still work
    auto result = Query(*db, "SELECT col_int FROM tae_scan('" +
                              ManifestPath("manifest_sorted.json") +
                              "') ORDER BY col_int ASC");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 8);
    CHECK(result->GetValue(0, 0) == Value::INTEGER(1));
    CHECK(result->GetValue(0, 7) == Value::INTEGER(400));
}

TEST_CASE("Scan: sort_column parsed from manifest", "[orderby]") {
    auto db = MakeDB();
    // Verify manifest with sort_column works for basic queries too
    auto result = Query(*db, "SELECT col_int, col_bool FROM tae_scan('" +
                              ManifestPath("manifest_sorted.json") +
                              "') ORDER BY col_int ASC");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 8);
    // First 4 rows from block 0: [1,2,3,4] with bools [T,F,T,F]
    CHECK(result->GetValue(0, 0) == Value::INTEGER(1));
    CHECK(result->GetValue(1, 0) == Value::BOOLEAN(true));
    CHECK(result->GetValue(0, 1) == Value::INTEGER(2));
    CHECK(result->GetValue(1, 1) == Value::BOOLEAN(false));
}

// ===================================================================
// Filter on non-projected column (filter_prune)
// ===================================================================

TEST_CASE("Scan: filter on non-projected column returns correct results", "[filter_prune]") {
    auto db = MakeDB();
    // Filter on col_str (not in SELECT), project col_int and col_dbl only
    auto result = Query(*db, "SELECT col_int, col_dbl FROM tae_scan('" +
                              ManifestPath("manifest.json") +
                              "') WHERE col_str = 'gamma' ORDER BY col_int");
    if (result->HasError()) { UNSCOPED_INFO("Error: " << result->GetError()); }
    REQUIRE_FALSE(result->HasError());
    // basic_3col: gamma is at row index 2 → col_int=30, col_dbl=3.3
    REQUIRE(result->RowCount() == 1);
    CHECK(result->GetValue(0, 0) == Value::INTEGER(30));
    CHECK(result->GetValue(1, 0) == Value::DOUBLE(3.3));
}

TEST_CASE("Scan: filter on non-projected numeric column", "[filter_prune]") {
    auto db = MakeDB();
    // Filter on col_int (not in SELECT), project col_str only
    auto result = Query(*db, "SELECT col_str FROM tae_scan('" +
                              ManifestPath("manifest.json") +
                              "') WHERE col_int > 50 ORDER BY col_str");
    if (result->HasError()) { UNSCOPED_INFO("Error: " << result->GetError()); }
    REQUIRE_FALSE(result->HasError());
    // basic_3col col_int values: [10,20,30,40,50,60,70,80]
    // col_str values: [alpha,beta,gamma,delta,epsilon,zeta,eta,theta]
    // > 50 → rows with col_int 60, 70, 80 → col_str: zeta, eta, theta
    // ORDER BY col_str → eta, theta, zeta
    REQUIRE(result->RowCount() == 3);
    CHECK(result->GetValue(0, 0).ToString() == "eta");
    CHECK(result->GetValue(0, 1).ToString() == "theta");
    CHECK(result->GetValue(0, 2).ToString() == "zeta");
}

TEST_CASE("Scan: filter on non-projected column with zone map skip", "[filter_prune]") {
    auto db = MakeDB();
    // Filter on col_int (not in SELECT), should use zone map for block skip
    auto result = Query(*db, "SELECT col_str FROM tae_scan('" +
                              ManifestPath("manifest.json") +
                              "') WHERE col_int = 999");
    if (result->HasError()) { UNSCOPED_INFO("Error: " << result->GetError()); }
    REQUIRE_FALSE(result->HasError());
    // 999 is outside all zone map ranges → 0 rows
    REQUIRE(result->RowCount() == 0);
}

// ===================================================================
// Statistics callback verification
// ===================================================================

TEST_CASE("Stats: MIN/MAX aggregates match zone map values", "[stats]") {
    auto db = MakeDB();
    // basic_3col: col_int [10..80], col_dbl [1.1..8.8]
    auto r1 = Query(*db, "SELECT MIN(col_int), MAX(col_int) FROM tae_scan('" +
                          ManifestPath("manifest.json") + "')");
    REQUIRE_FALSE(r1->HasError());
    CHECK(r1->GetValue(0, 0).GetValue<int32_t>() == 10);
    CHECK(r1->GetValue(1, 0).GetValue<int32_t>() == 80);

    auto r2 = Query(*db, "SELECT MIN(col_dbl), MAX(col_dbl) FROM tae_scan('" +
                          ManifestPath("manifest.json") + "')");
    REQUIRE_FALSE(r2->HasError());
    CHECK(std::abs(r2->GetValue(0, 0).GetValue<double>() - 1.1) < 0.01);
    CHECK(std::abs(r2->GetValue(1, 0).GetValue<double>() - 8.8) < 0.01);
}

TEST_CASE("Stats: string MIN/MAX aggregates", "[stats]") {
    auto db = MakeDB();
    // basic_3col col_str: [alpha, beta, gamma, delta, epsilon, zeta, eta, theta]
    // sorted: alpha < beta < delta < epsilon < eta < gamma < theta < zeta
    auto result = Query(*db, "SELECT MIN(col_str), MAX(col_str) FROM tae_scan('" +
                              ManifestPath("manifest.json") + "')");
    REQUIRE_FALSE(result->HasError());
    CHECK(result->GetValue(0, 0).ToString() == "alpha");
    CHECK(result->GetValue(1, 0).ToString() == "zeta");
}

TEST_CASE("Stats: null column aggregates", "[stats]") {
    auto db = MakeDB();
    // with_nulls: col_int [10, NULL, 30, NULL, 50, 60], col_str with NULLs
    auto r1 = Query(*db, "SELECT MIN(col_int), MAX(col_int), COUNT(*), COUNT(col_int) "
                          "FROM tae_scan('" + ManifestPath("manifest_nulls.json") + "')");
    if (r1->HasError()) { UNSCOPED_INFO("Error: " << r1->GetError()); }
    REQUIRE_FALSE(r1->HasError());
    CHECK(r1->GetValue(0, 0).GetValue<int32_t>() == 10);   // MIN
    CHECK(r1->GetValue(1, 0).GetValue<int32_t>() == 60);   // MAX
    CHECK(r1->GetValue(2, 0).GetValue<int64_t>() == 6);    // COUNT(*)
    CHECK(r1->GetValue(3, 0).GetValue<int64_t>() == 4);    // COUNT(col_int) excludes NULLs
}

TEST_CASE("Stats: multifile min/max merge", "[stats]") {
    auto db = MakeDB();
    // multifile = basic_3col [10..80] + basic_3col_part2 [100..400]
    auto result = Query(*db, "SELECT MIN(col_int), MAX(col_int) FROM tae_scan('" +
                              ManifestPath("manifest_multifile.json") + "')");
    REQUIRE_FALSE(result->HasError());
    CHECK(result->GetValue(0, 0).GetValue<int32_t>() == 10);
    CHECK(result->GetValue(1, 0).GetValue<int32_t>() == 400);
}

// ===================================================================
// Object-level partition pruning tests
// ===================================================================

TEST_CASE("Partition prune: filter skips entire object via zone maps", "[partition_prune]") {
    auto db = MakeDB();
    // multifile: basic_3col has col_int [10..80], part2 has [100..400]
    // Filter col_int > 90 should skip the first object entirely
    auto result = Query(*db,
        "SELECT col_int FROM tae_scan('" +
        ManifestPath("manifest_multifile.json") + "') WHERE col_int > 90 ORDER BY col_int");
    REQUIRE_FALSE(result->HasError());
    CHECK(result->RowCount() == 4);
    CHECK(result->GetValue(0, 0).GetValue<int32_t>() == 100);
    CHECK(result->GetValue(0, 1).GetValue<int32_t>() == 200);
    CHECK(result->GetValue(0, 2).GetValue<int32_t>() == 300);
    CHECK(result->GetValue(0, 3).GetValue<int32_t>() == 400);
}

TEST_CASE("Partition prune: filter passes all objects", "[partition_prune]") {
    auto db = MakeDB();
    // Filter col_int > 0 should pass both objects (min 10 in first, 100 in second)
    auto result = Query(*db,
        "SELECT COUNT(*) FROM tae_scan('" +
        ManifestPath("manifest_multifile.json") + "') WHERE col_int > 0");
    REQUIRE_FALSE(result->HasError());
    CHECK(result->GetValue(0, 0).GetValue<int64_t>() == 12);
}

TEST_CASE("Partition prune: filter skips all objects", "[partition_prune]") {
    auto db = MakeDB();
    // Filter col_int > 9999 should skip both objects (max 80 and 400)
    auto result = Query(*db,
        "SELECT COUNT(*) FROM tae_scan('" +
        ManifestPath("manifest_multifile.json") + "') WHERE col_int > 9999");
    REQUIRE_FALSE(result->HasError());
    CHECK(result->GetValue(0, 0).GetValue<int64_t>() == 0);
}

TEST_CASE("Partition prune: manifest zone_map fast path skips object", "[partition_prune]") {
    auto db = MakeDB();
    // Uses manifest_multifile_zm.json which has sort_key zone maps.
    // Filter col_int > 90: first object zm [10..80] fails → skipped via fast path (no ReadMeta)
    auto result = Query(*db,
        "SELECT col_int FROM tae_scan('" +
        ManifestPath("manifest_multifile_zm.json") + "') WHERE col_int > 90 ORDER BY col_int");
    REQUIRE_FALSE(result->HasError());
    CHECK(result->RowCount() == 4);
    CHECK(result->GetValue(0, 0).GetValue<int32_t>() == 100);
    CHECK(result->GetValue(0, 3).GetValue<int32_t>() == 400);
}

TEST_CASE("Partition prune: manifest zone_map passes all objects", "[partition_prune]") {
    auto db = MakeDB();
    // Filter col_int > 5: both objects pass (zm min 10 and 100 both > 5)
    auto result = Query(*db,
        "SELECT COUNT(*) FROM tae_scan('" +
        ManifestPath("manifest_multifile_zm.json") + "') WHERE col_int > 5");
    REQUIRE_FALSE(result->HasError());
    CHECK(result->GetValue(0, 0).GetValue<int64_t>() == 12);
}

TEST_CASE("Partition prune: manifest zone_map skips all objects", "[partition_prune]") {
    auto db = MakeDB();
    // Filter col_int > 9999: both zms fail
    auto result = Query(*db,
        "SELECT COUNT(*) FROM tae_scan('" +
        ManifestPath("manifest_multifile_zm.json") + "') WHERE col_int > 9999");
    REQUIRE_FALSE(result->HasError());
    CHECK(result->GetValue(0, 0).GetValue<int64_t>() == 0);
}
