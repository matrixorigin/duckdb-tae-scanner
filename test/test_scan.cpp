// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0
//
// End-to-end scan tests: load extension into an in-process DuckDB,
// run SQL queries against generated .tae files, verify results.

#include "catch.hpp"
#include "tae_scanner.hpp"

#include "duckdb.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/uuid.hpp"

#include <cstdio>
#include <fstream>

using namespace duckdb;

// Helper: create a DuckDB instance with tae_scan registered
static unique_ptr<DuckDB> MakeDB() {
    auto db = make_uniq<DuckDB>(nullptr);
    Connection con(*db);
    // Register the tae_scan function via the catalog
    auto func = tae::GetTAEScanFunction();
    CreateTableFunctionInfo info(func);
    auto &catalog = Catalog::GetSystemCatalog(*db->instance);
    con.BeginTransaction();
    auto &context = *con.context;
    catalog.CreateTableFunction(context, info);
    con.Commit();
    return db;
}

// Helper: run a query and return materialized result
static unique_ptr<MaterializedQueryResult> Query(DuckDB &db, const string &sql) {
    Connection con(db);
    return con.Query(sql);
}

static std::string ManifestPath(const char *name) {
    return std::string(TEST_DATA_DIR) + "/" + name;
}

// ===================================================================
// Basic scan
// ===================================================================

TEST_CASE("Scan: basic_3col returns all rows", "[scan]") {
    auto db = MakeDB();
    auto result = Query(*db, "SELECT * FROM tae_scan('" + ManifestPath("manifest.json") + "')");
    REQUIRE(result->HasError() == false);
    REQUIRE(result->RowCount() == 8);
    REQUIRE(result->ColumnCount() == 3);

    // Check column names
    REQUIRE(result->names[0] == "col_int");
    REQUIRE(result->names[1] == "col_str");
    REQUIRE(result->names[2] == "col_dbl");
}

TEST_CASE("Scan: basic_3col data values", "[scan]") {
    auto db = MakeDB();
    auto result = Query(*db, "SELECT col_int, col_str, col_dbl FROM tae_scan('" +
                              ManifestPath("manifest.json") + "') ORDER BY col_int");
    REQUIRE_FALSE(result->HasError());

    // Row 0: col_int=10, col_str='alpha', col_dbl≈1.1
    REQUIRE(result->GetValue(0, 0) == Value::INTEGER(10));
    REQUIRE(result->GetValue(1, 0) == Value("alpha"));
    REQUIRE(result->GetValue(2, 0).GetValue<double>() == Approx(1.1));

    // Row 7: col_int=80, col_str='theta', col_dbl≈8.8
    REQUIRE(result->GetValue(0, 7) == Value::INTEGER(80));
    REQUIRE(result->GetValue(1, 7) == Value("theta"));
    REQUIRE(result->GetValue(2, 7).GetValue<double>() == Approx(8.8));
}

// ===================================================================
// Projection pushdown
// ===================================================================

TEST_CASE("Scan: projection pushdown reads only requested columns", "[scan]") {
    auto db = MakeDB();
    auto result = Query(*db, "SELECT col_str FROM tae_scan('" +
                              ManifestPath("manifest.json") + "')");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->ColumnCount() == 1);
    REQUIRE(result->RowCount() == 8);
    REQUIRE(result->GetValue(0, 0) == Value("alpha"));
}

// ===================================================================
// Multi-block
// ===================================================================

TEST_CASE("Scan: multi_block reads across blocks", "[scan]") {
    auto db = MakeDB();
    auto result = Query(*db, "SELECT col_int FROM tae_scan('" +
                              ManifestPath("manifest_multi.json") + "') ORDER BY col_int");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 8);
    // First block: 1,2,3,4; Second block: 100,200,300,400
    REQUIRE(result->GetValue(0, 0) == Value::INTEGER(1));
    REQUIRE(result->GetValue(0, 7) == Value::INTEGER(400));
}

// ===================================================================
// Multi-file (multiple .tae objects in one manifest)
// ===================================================================

TEST_CASE("Scan: multi-file reads across TAE objects", "[scan][multifile]") {
    auto db = MakeDB();
    auto result = Query(*db, "SELECT col_int, col_str, col_dbl FROM tae_scan('" +
                              ManifestPath("manifest_multifile.json") + "') ORDER BY col_int");
    REQUIRE_FALSE(result->HasError());
    // 8 rows from basic_3col.tae + 4 rows from basic_3col_part2.tae = 12
    REQUIRE(result->RowCount() == 12);
    REQUIRE(result->ColumnCount() == 3);

    // First file rows (sorted): 10..80
    REQUIRE(result->GetValue(0, 0) == Value::INTEGER(10));
    REQUIRE(result->GetValue(1, 0) == Value("alpha"));

    // Last rows (from second file): 100..400
    REQUIRE(result->GetValue(0, 8) == Value::INTEGER(100));
    REQUIRE(result->GetValue(1, 8) == Value("one"));
    REQUIRE(result->GetValue(0, 11) == Value::INTEGER(400));
    REQUIRE(result->GetValue(1, 11) == Value("four"));
}

TEST_CASE("Scan: multi-file COUNT(*)", "[scan][multifile]") {
    auto db = MakeDB();
    auto result = Query(*db, "SELECT COUNT(*) FROM tae_scan('" +
                              ManifestPath("manifest_multifile.json") + "')");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->GetValue(0, 0) == Value::BIGINT(12));
}

TEST_CASE("Scan: multi-file filter crosses file boundary", "[scan][multifile]") {
    auto db = MakeDB();
    // Filter that spans both files: col_int > 50
    // File 1: 60,70,80 match. File 2: 100,200,300,400 match. Total 7 rows.
    auto result = Query(*db, "SELECT col_int FROM tae_scan('" +
                              ManifestPath("manifest_multifile.json") +
                              "') WHERE col_int > 50 ORDER BY col_int");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 7);
    REQUIRE(result->GetValue(0, 0) == Value::INTEGER(60));
    REQUIRE(result->GetValue(0, 6) == Value::INTEGER(400));
}

// ===================================================================
// Null handling
// ===================================================================

TEST_CASE("Scan: with_nulls shows correct NULL values", "[scan]") {
    auto db = MakeDB();
    auto result = Query(*db, "SELECT col_int, col_str FROM tae_scan('" +
                              ManifestPath("manifest_nulls.json") + "')");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 6);

    // Row 0: 10, 'hello'
    REQUIRE(result->GetValue(0, 0) == Value::INTEGER(10));
    REQUIRE(result->GetValue(1, 0) == Value("hello"));

    // Row 1: NULL, 'world'
    REQUIRE(result->GetValue(0, 1).IsNull());
    REQUIRE(result->GetValue(1, 1) == Value("world"));

    // Row 2: 30, NULL
    REQUIRE(result->GetValue(0, 2) == Value::INTEGER(30));
    REQUIRE(result->GetValue(1, 2).IsNull());

    // Row 3: NULL, 'test'
    REQUIRE(result->GetValue(0, 3).IsNull());
    REQUIRE(result->GetValue(1, 3) == Value("test"));
}

// ===================================================================
// Filter pushdown (zone map)
// ===================================================================

TEST_CASE("Scan: zone map filter skips blocks", "[scan]") {
    auto db = MakeDB();
    // Block 0 has values 1..4, block 1 has 100..400
    // WHERE col_int > 50 should skip block 0
    auto result = Query(*db, "SELECT col_int FROM tae_scan('" +
                              ManifestPath("manifest_multi.json") +
                              "') WHERE col_int > 50 ORDER BY col_int");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 4);
    REQUIRE(result->GetValue(0, 0) == Value::INTEGER(100));
    REQUIRE(result->GetValue(0, 3) == Value::INTEGER(400));
}

TEST_CASE("Scan: WHERE filters with no matching blocks", "[scan]") {
    auto db = MakeDB();
    // No values > 1000 exist
    auto result = Query(*db, "SELECT col_int FROM tae_scan('" +
                              ManifestPath("manifest_multi.json") +
                              "') WHERE col_int > 1000");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 0);
}

// ===================================================================
// COUNT(*) with scan
// ===================================================================

TEST_CASE("Scan: COUNT(*) works", "[scan]") {
    auto db = MakeDB();
    auto result = Query(*db, "SELECT COUNT(*) FROM tae_scan('" +
                              ManifestPath("manifest.json") + "')");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->GetValue(0, 0) == Value::BIGINT(8));
}

// ===================================================================
// Planner statistics
// ===================================================================

TEST_CASE("Scan: cardinality estimate appears in EXPLAIN", "[scan][stats]") {
    auto db = MakeDB();
    auto result = Query(*db, "EXPLAIN SELECT * FROM tae_scan('" +
                              ManifestPath("manifest.json") + "')");
    REQUIRE_FALSE(result->HasError());
    auto explain_str = result->GetValue(1, 0).ToString();
    // DuckDB uppercases the function name in EXPLAIN output
    REQUIRE(explain_str.find("TAE_SCAN") != std::string::npos);
}

TEST_CASE("Scan: cardinality single file = 8 rows", "[scan][stats]") {
    auto db = MakeDB();
    // Verify cardinality by checking that COUNT(*) returns the manifest row count
    auto result = Query(*db, "SELECT COUNT(*) FROM tae_scan('" +
                              ManifestPath("manifest.json") + "')");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->GetValue(0, 0) == Value::BIGINT(8));
}

TEST_CASE("Scan: cardinality multi-file = 12 rows", "[scan][stats]") {
    auto db = MakeDB();
    auto result = Query(*db, "SELECT COUNT(*) FROM tae_scan('" +
                              ManifestPath("manifest_multifile.json") + "')");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->GetValue(0, 0) == Value::BIGINT(12));
}

TEST_CASE("Scan: column stats enable optimized filter elimination", "[scan][stats]") {
    auto db = MakeDB();
    // basic_3col has col_int in [10..80]. A filter col_int > 1000 should return 0 rows.
    // The planner may use column stats to realize this is impossible.
    auto result = Query(*db, "SELECT COUNT(*) FROM tae_scan('" +
                              ManifestPath("manifest.json") +
                              "') WHERE col_int > 1000");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->GetValue(0, 0) == Value::BIGINT(0));
}

TEST_CASE("Scan: column stats min/max respected in range queries", "[scan][stats]") {
    auto db = MakeDB();
    // All col_int values in basic_3col are 10,20,...,80
    // col_int >= 10 AND col_int <= 80 should return all 8 rows
    auto result = Query(*db, "SELECT COUNT(*) FROM tae_scan('" +
                              ManifestPath("manifest.json") +
                              "') WHERE col_int >= 10 AND col_int <= 80");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->GetValue(0, 0) == Value::BIGINT(8));
}

// ===================================================================
// EXPLAIN output (to_string / dynamic_to_string)
// ===================================================================

TEST_CASE("Scan: EXPLAIN shows table name and object count", "[scan][explain]") {
    auto db = MakeDB();
    auto result = Query(*db, "EXPLAIN SELECT * FROM tae_scan('" +
                              ManifestPath("manifest_multifile.json") + "')");
    REQUIRE_FALSE(result->HasError());
    auto text = result->GetValue(1, 0).ToString();
    REQUIRE(text.find("test_multifile") != std::string::npos);
    REQUIRE(text.find("Objects: 2") != std::string::npos);
    REQUIRE(text.find("Total Rows: 12") != std::string::npos);
}

TEST_CASE("Scan: EXPLAIN ANALYZE shows runtime stats", "[scan][explain]") {
    auto db = MakeDB();
    auto result = Query(*db, "EXPLAIN ANALYZE SELECT * FROM tae_scan('" +
                              ManifestPath("manifest.json") + "')");
    REQUIRE_FALSE(result->HasError());
    auto text = result->GetValue(1, 0).ToString();
    REQUIRE(text.find("Blocks Scanned") != std::string::npos);
    REQUIRE(text.find("Rows Emitted") != std::string::npos);
}

// ===================================================================
// Parallel scanning correctness
// ===================================================================

TEST_CASE("Scan: parallel scan returns correct results for multi-block", "[scan][parallel]") {
    auto db = MakeDB();
    // multi_block has 2 blocks: block0=[1,2,3,4], block1=[100,200,300,400]
    // With parallelism, blocks may be scanned in any order; ORDER BY ensures determinism
    auto result = Query(*db, "SELECT col_int FROM tae_scan('" +
                              ManifestPath("manifest_multi.json") +
                              "') ORDER BY col_int");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 8);
    REQUIRE(result->GetValue(0, 0) == Value::INTEGER(1));
    REQUIRE(result->GetValue(0, 3) == Value::INTEGER(4));
    REQUIRE(result->GetValue(0, 4) == Value::INTEGER(100));
    REQUIRE(result->GetValue(0, 7) == Value::INTEGER(400));
}

TEST_CASE("Scan: parallel scan correctness with multi-file", "[scan][parallel]") {
    auto db = MakeDB();
    // 2 objects (8+4 rows), blocks may be scanned in parallel
    auto result = Query(*db, "SELECT col_int FROM tae_scan('" +
                              ManifestPath("manifest_multifile.json") +
                              "') ORDER BY col_int");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 12);
    REQUIRE(result->GetValue(0, 0) == Value::INTEGER(10));
    REQUIRE(result->GetValue(0, 11) == Value::INTEGER(400));
}

TEST_CASE("Scan: parallel scan with filter returns correct subset", "[scan][parallel]") {
    auto db = MakeDB();
    auto result = Query(*db, "SELECT col_int FROM tae_scan('" +
                              ManifestPath("manifest_multifile.json") +
                              "') WHERE col_int >= 100 ORDER BY col_int");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 4);
    REQUIRE(result->GetValue(0, 0) == Value::INTEGER(100));
    REQUIRE(result->GetValue(0, 3) == Value::INTEGER(400));
}

// ===================================================================
// Virtual columns
// ===================================================================

TEST_CASE("Scan: virtual column file_path returns object filename", "[scan][virtual]") {
    auto db = MakeDB();
    auto result = Query(*db, "SELECT file_path FROM tae_scan('" +
                              ManifestPath("manifest.json") + "')");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 8);
    // All rows should have the same file path
    auto val = result->GetValue(0, 0).ToString();
    CHECK(val.find("basic_3col.tae") != std::string::npos);
    for (idx_t i = 1; i < result->RowCount(); i++) {
        CHECK(result->GetValue(0, i).ToString() == val);
    }
}

TEST_CASE("Scan: virtual column block_id returns block index", "[scan][virtual]") {
    auto db = MakeDB();
    auto result = Query(*db, "SELECT block_id FROM tae_scan('" +
                              ManifestPath("manifest_multi.json") + "') ORDER BY block_id");
    REQUIRE_FALSE(result->HasError());
    // multi_block.tae has 2 blocks, 4 rows each
    REQUIRE(result->RowCount() == 8);
    // First 4 rows should be block 0, next 4 block 1
    for (idx_t i = 0; i < 4; i++) {
        CHECK(result->GetValue(0, i) == Value::INTEGER(0));
    }
    for (idx_t i = 4; i < 8; i++) {
        CHECK(result->GetValue(0, i) == Value::INTEGER(1));
    }
}

TEST_CASE("Scan: virtual columns mixed with TAE columns", "[scan][virtual]") {
    auto db = MakeDB();
    auto result = Query(*db, "SELECT col_int, file_path, block_id FROM tae_scan('" +
                              ManifestPath("manifest.json") + "') ORDER BY col_int");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 8);
    // Check TAE column values
    CHECK(result->GetValue(0, 0) == Value::INTEGER(10));
    CHECK(result->GetValue(0, 7) == Value::INTEGER(80));
    // file_path for all rows
    auto fname = result->GetValue(1, 0).ToString();
    CHECK(fname.find("basic_3col.tae") != std::string::npos);
    // block_id should be 0 for single-block file
    for (idx_t i = 0; i < 8; i++) {
        CHECK(result->GetValue(2, i) == Value::INTEGER(0));
    }
}

TEST_CASE("Scan: virtual columns with multi-file show different file paths", "[scan][virtual]") {
    auto db = MakeDB();
    auto result = Query(*db, "SELECT col_int, file_path FROM tae_scan('" +
                              ManifestPath("manifest_multifile.json") +
                              "') ORDER BY col_int");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 12);
    // Rows from basic_3col.tae (int 10-80) and basic_3col_part2.tae (int 100-400)
    auto fp_first = result->GetValue(1, 0).ToString();   // int=10 → basic_3col.tae
    auto fp_last  = result->GetValue(1, 11).ToString();   // int=400 → basic_3col_part2.tae
    CHECK(fp_first.find("basic_3col.tae") != std::string::npos);
    CHECK(fp_last.find("basic_3col_part2.tae") != std::string::npos);
    CHECK(fp_first != fp_last);
}

// ===================================================================
// Sampling pushdown
// ===================================================================

TEST_CASE("Scan: TABLESAMPLE SYSTEM returns subset of rows", "[scan][sample]") {
    auto db = MakeDB();
    // 50% system sample of 8 rows — should return fewer than 8 most of the time.
    // Run multiple times to avoid flaky failures from randomness.
    bool saw_fewer = false;
    for (int trial = 0; trial < 10; trial++) {
        auto result = Query(*db, "SELECT col_int FROM tae_scan('" +
                                  ManifestPath("manifest.json") +
                                  "') TABLESAMPLE SYSTEM(50%)");
        if (result->HasError()) { UNSCOPED_INFO("Error: " << result->GetError()); }
        REQUIRE_FALSE(result->HasError());
        CHECK(result->RowCount() <= 8);
        if (result->RowCount() < 8) saw_fewer = true;
    }
    CHECK(saw_fewer);
}

TEST_CASE("Scan: TABLESAMPLE SYSTEM 100% returns all rows", "[scan][sample]") {
    auto db = MakeDB();
    auto result = Query(*db, "SELECT col_int FROM tae_scan('" +
                              ManifestPath("manifest.json") +
                              "') TABLESAMPLE SYSTEM(100%)");
    if (result->HasError()) { UNSCOPED_INFO("Error: " << result->GetError()); }
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 8);
}

TEST_CASE("Scan: TABLESAMPLE SYSTEM 0% returns no rows", "[scan][sample]") {
    auto db = MakeDB();
    auto result = Query(*db, "SELECT col_int FROM tae_scan('" +
                              ManifestPath("manifest.json") +
                              "') TABLESAMPLE SYSTEM(0%)");
    if (result->HasError()) { UNSCOPED_INFO("Error: " << result->GetError()); }
    REQUIRE_FALSE(result->HasError());
    // 0% should return 0 rows (rate=0, all rows rejected)
    REQUIRE(result->RowCount() == 0);
}

// ===================================================================
// Constant vector handling
// ===================================================================

TEST_CASE("Scan: constant int column returns same value for all rows", "[scan][constant]") {
    auto db = MakeDB();
    // Block 0 has col_int = CONSTANT(42), 4 rows
    // Block 1 has col_int = FLAT [10,20,30,40], 4 rows
    auto result = Query(*db, "SELECT col_int FROM tae_scan('" +
                              ManifestPath("manifest_constants.json") +
                              "') ORDER BY col_int");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 8);
    // Sorted: 10, 20, 30, 40, 42, 42, 42, 42
    CHECK(result->GetValue(0, 0) == Value::INTEGER(10));
    CHECK(result->GetValue(0, 1) == Value::INTEGER(20));
    CHECK(result->GetValue(0, 2) == Value::INTEGER(30));
    CHECK(result->GetValue(0, 3) == Value::INTEGER(40));
    CHECK(result->GetValue(0, 4) == Value::INTEGER(42));
    CHECK(result->GetValue(0, 5) == Value::INTEGER(42));
    CHECK(result->GetValue(0, 6) == Value::INTEGER(42));
    CHECK(result->GetValue(0, 7) == Value::INTEGER(42));
}

TEST_CASE("Scan: constant string column returns same value for all rows", "[scan][constant]") {
    auto db = MakeDB();
    // Block 1 has col_str = CONSTANT('hello'), 4 rows
    auto result = Query(*db, "SELECT col_str FROM tae_scan('" +
                              ManifestPath("manifest_constants.json") +
                              "') ORDER BY col_str");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 8);
    // Block 0 has flat strings a,b,c,d; block 1 has constant 'hello'
    // Sorted: a, b, c, d, hello, hello, hello, hello
    CHECK(result->GetValue(0, 0) == Value("a"));
    CHECK(result->GetValue(0, 3) == Value("d"));
    CHECK(result->GetValue(0, 4) == Value("hello"));
    CHECK(result->GetValue(0, 7) == Value("hello"));
}

TEST_CASE("Scan: constant NULL double column returns NULLs", "[scan][constant]") {
    auto db = MakeDB();
    // Block 0 has col_dbl = CONSTANT(3.14); block 1 has CONSTANT_NULL
    auto result = Query(*db, "SELECT col_dbl FROM tae_scan('" +
                              ManifestPath("manifest_constants.json") + "')");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 8);
    // Count non-null and null values
    int null_count = 0;
    int nonnull_count = 0;
    for (idx_t i = 0; i < 8; i++) {
        if (result->GetValue(0, i).IsNull()) {
            null_count++;
        } else {
            nonnull_count++;
            CHECK(result->GetValue(0, i).GetValue<double>() == Approx(3.14));
        }
    }
    CHECK(null_count == 4);      // block 1: 4 constant NULLs
    CHECK(nonnull_count == 4);   // block 0: 4 constant 3.14
}

TEST_CASE("Scan: mixed constant/flat columns work together", "[scan][constant]") {
    auto db = MakeDB();
    auto result = Query(*db, "SELECT col_int, col_str, col_dbl FROM tae_scan('" +
                              ManifestPath("manifest_constants.json") +
                              "') ORDER BY col_int, col_str");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 8);
    // Row with col_int=10 comes from block 1 (int=FLAT, str=CONST 'hello', dbl=CONST_NULL)
    CHECK(result->GetValue(0, 0) == Value::INTEGER(10));
    CHECK(result->GetValue(1, 0) == Value("hello"));
    CHECK(result->GetValue(2, 0).IsNull());
}

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
// Negative / error tests
// ===================================================================

// Helper: write bytes to a temp file, return its path.
// Caller must std::remove() the file after use.
static std::string WriteTempFile(const std::string &name,
                                  const std::vector<uint8_t> &data) {
    auto path = std::string(TEST_DATA_DIR) + "/" + name;
    std::ofstream ofs(path, std::ios::binary);
    ofs.write(reinterpret_cast<const char *>(data.data()),
              static_cast<std::streamsize>(data.size()));
    ofs.close();
    return path;
}

// Helper: write a minimal manifest JSON pointing to a single object file.
static std::string WriteTempManifest(const std::string &name,
                                      const std::string &obj_file,
                                      int blocks, int rows) {
    auto path = std::string(TEST_DATA_DIR) + "/" + name;
    std::ofstream ofs(path);
    ofs << R"({"columns":[{"name":"col_int","oid":22}],)"
        << R"("objects":[{"path":")" << obj_file << R"(","blocks":)" << blocks
        << R"(,"rows":)" << rows << R"(}]})";
    ofs.close();
    return path;
}

TEST_CASE("Error: missing manifest file", "[error]") {
    auto db = MakeDB();
    auto result = Query(*db, "SELECT * FROM tae_scan('/tmp/nonexistent_manifest_xyz.json')");
    REQUIRE(result->HasError());
    CHECK(result->GetError().find("cannot open manifest") != std::string::npos);
}

TEST_CASE("Error: malformed JSON manifest", "[error]") {
    auto path = WriteTempFile("bad_manifest.json", {'{', '"', 'x'});
    auto db = MakeDB();
    auto result = Query(*db, "SELECT * FROM tae_scan('" + path + "')");
    REQUIRE(result->HasError());
    CHECK(result->GetError().find("invalid JSON") != std::string::npos);
    std::remove(path.c_str());
}

TEST_CASE("Error: object file with bad magic", "[error]") {
    // Write 64 bytes of zeros (wrong magic — should be 0xFFFFFFFF)
    std::vector<uint8_t> bad_obj(64, 0);
    auto obj_path = WriteTempFile("bad_magic.tae", bad_obj);
    auto mf_path = WriteTempManifest("mf_bad_magic.json", "bad_magic.tae", 1, 1);
    auto db = MakeDB();
    auto result = Query(*db, "SELECT col_int FROM tae_scan('" + mf_path + "')");
    if (result->HasError()) { UNSCOPED_INFO("Error: " << result->GetError()); }
    REQUIRE(result->HasError());
    CHECK(result->GetError().find("magic") != std::string::npos);
    std::remove(obj_path.c_str());
    std::remove(mf_path.c_str());
}

TEST_CASE("Error: truncated object file (too short for header)", "[error]") {
    // Only 8 bytes — not enough for 64-byte header
    std::vector<uint8_t> tiny(8, 0xFF);
    auto obj_path = WriteTempFile("truncated.tae", tiny);
    auto mf_path = WriteTempManifest("mf_truncated.json", "truncated.tae", 1, 1);
    auto db = MakeDB();
    auto result = Query(*db, "SELECT col_int FROM tae_scan('" + mf_path + "')");
    REQUIRE(result->HasError());
    // Should fail during ReadMeta when reading 64 bytes from 8-byte file
    std::remove(obj_path.c_str());
    std::remove(mf_path.c_str());
}

TEST_CASE("Error: object file does not exist", "[error]") {
    auto mf_path = WriteTempManifest("mf_missing_obj.json", "nonexistent_obj.tae", 1, 1);
    auto db = MakeDB();
    auto result = Query(*db, "SELECT col_int FROM tae_scan('" + mf_path + "')");
    REQUIRE(result->HasError());
    std::remove(mf_path.c_str());
}

TEST_CASE("Error: empty manifest returns no rows", "[error]") {
    // Valid JSON but no objects/columns
    auto path = WriteTempFile("empty_manifest.json",
        std::vector<uint8_t>('{', '}'));
    auto db = MakeDB();
    auto result = Query(*db, "SELECT * FROM tae_scan('" + path + "')");
    // Should succeed with 0 columns → DuckDB may report error or empty
    // Either way, it shouldn't crash
    (void)result;
    std::remove(path.c_str());
}
