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
