// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0
//
// Shared helpers for tae_scanner end-to-end tests.

#pragma once

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
