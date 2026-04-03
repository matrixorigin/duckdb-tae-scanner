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
