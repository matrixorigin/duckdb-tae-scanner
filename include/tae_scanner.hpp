// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0
//
// DuckDB TableFunction for scanning MO TAE objects from local filesystem.
//
// Usage (from DuckDB):
//   SELECT * FROM tae_scan('/path/to/manifest.json');
//
// Supports predicate pushdown via zone maps: comparisons on columns
// are evaluated against block-level min/max to skip irrelevant blocks.

#pragma once

#include "duckdb.hpp"
#include "tae_object_reader.hpp"
#include "tae_zonemap.hpp"
#include <memory>
#include <string>
#include <vector>

namespace tae {

// ---------------------------------------------------------------------------
// Manifest: list of objects for a table
// ---------------------------------------------------------------------------
struct TAEObjectInfo {
    std::string file_path;   // relative to data_dir (the ObjectName string)
    uint32_t    rows;
    uint32_t    blocks;
    uint32_t    size_bytes;
};

// ---------------------------------------------------------------------------
// Pushed-down filter: one comparison predicate on one column
// ---------------------------------------------------------------------------
struct PushedFilter {
    uint16_t    col_idx;       // index into projected columns
    uint16_t    seqnum;        // TAE column seqnum
    uint8_t     mo_type_oid;   // MO type for dispatching comparison
    FilterOp    op;
    // Constant value stored as raw bytes (same encoding as zone map)
    std::vector<uint8_t> constant;
    uint32_t    const_len;     // for string types: actual string length
};

// ---------------------------------------------------------------------------
// Bind data — schema + object list, computed once per query (const after bind)
// ---------------------------------------------------------------------------
struct TAEScanBindData : public duckdb::TableFunctionData {
    std::string                    data_dir;
    std::string                    db_name;
    std::string                    table_name;

    // Schema: column names and DuckDB types (in table order)
    std::vector<std::string>       all_col_names;
    std::vector<duckdb::LogicalType> all_col_types;
    std::vector<uint8_t>           all_col_mo_oids; // MO type OID per column

    // Object list
    std::vector<TAEObjectInfo>     objects;
    duckdb::idx_t                  total_rows = 0; // sum of all objects' row counts
};

// ---------------------------------------------------------------------------
// Init state — per-thread scan state (mutable during scan)
// ---------------------------------------------------------------------------
struct TAEScanState : public duckdb::GlobalTableFunctionState {
    // Projection: which columns the query actually needs
    std::vector<duckdb::idx_t>     projected_col_indices; // into all_col_*
    std::vector<uint16_t>          read_seqnums;

    // Pushed-down filters (populated from DuckDB TableFilterSet)
    std::vector<PushedFilter>      filters;

    // Current position
    duckdb::idx_t current_object = 0;
    duckdb::idx_t current_block  = 0;

    // Current reader (one per object file)
    std::unique_ptr<TAEObjectReader> reader;

    // Statistics
    uint64_t blocks_scanned = 0;
    uint64_t blocks_skipped = 0;

    duckdb::idx_t MaxThreads() const override { return 1; }
};

// ---------------------------------------------------------------------------
// Public API: register the tae_scan table function
// ---------------------------------------------------------------------------
duckdb::TableFunction GetTAEScanFunction();

} // namespace tae
