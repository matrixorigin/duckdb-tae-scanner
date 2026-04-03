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
#include <atomic>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace tae {

// Virtual column IDs (must be >= VIRTUAL_COLUMN_START = 2^63)
static constexpr duckdb::column_t VCOL_FILENAME  = UINT64_C(9223372036854775808); // 2^63
static constexpr duckdb::column_t VCOL_BLOCK_ID  = UINT64_C(9223372036854775809); // 2^63 + 1

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
// Work unit: one block within one object
// ---------------------------------------------------------------------------
struct WorkUnit {
    uint32_t object_idx;
    uint32_t block_idx;
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
    duckdb::idx_t                  total_rows = 0;   // sum of all objects' row counts
    duckdb::idx_t                  total_blocks = 0;  // sum of all objects' block counts
};

// ---------------------------------------------------------------------------
// Global state — shared work dispatcher (thread-safe)
// ---------------------------------------------------------------------------

// Describes what each output column slot holds
struct OutputColumnInfo {
    enum Kind { TAE_COLUMN, VCOL_FILENAME, VCOL_BLOCK_ID };
    Kind         kind;
    duckdb::idx_t tae_col_idx; // only valid for TAE_COLUMN
};

struct TAEScanState : public duckdb::GlobalTableFunctionState {
    // Per output column: what to fill (TAE column or virtual column)
    std::vector<OutputColumnInfo>   output_map;

    // TAE column seqnums to read (only for TAE columns in output_map)
    std::vector<uint16_t>          read_seqnums;

    // Pushed-down filters (populated from DuckDB TableFilterSet)
    std::vector<PushedFilter>      filters;

    // Sampling pushdown
    double                         sample_rate = 1.0;  // 1.0 = no sampling
    bool                           do_sample = false;

    // Flattened work queue: all (object_idx, block_idx) pairs
    std::vector<WorkUnit>          work_units;
    std::atomic<duckdb::idx_t>     next_work_unit{0};

    // Statistics / progress (atomics for thread safety)
    std::atomic<uint64_t>          blocks_scanned{0};
    std::atomic<uint64_t>          blocks_skipped{0};
    std::atomic<uint64_t>          rows_emitted{0};

    duckdb::idx_t MaxThreads() const override {
        return work_units.empty() ? 1 : work_units.size();
    }
};

// ---------------------------------------------------------------------------
// Local state — per-thread reader and resources
// ---------------------------------------------------------------------------
struct TAEScanLocalState : public duckdb::LocalTableFunctionState {
    // Per-thread reader (avoids locking on file I/O)
    std::unique_ptr<TAEObjectReader> reader;
    uint32_t                         reader_object_idx = UINT32_MAX;

    // Per-thread RNG for Bernoulli sampling
    std::mt19937_64                  rng{std::random_device{}()};
    std::uniform_real_distribution<double> dist{0.0, 1.0};
};

// ---------------------------------------------------------------------------
// Public API: register the tae_scan table function
// ---------------------------------------------------------------------------
duckdb::TableFunction GetTAEScanFunction();

} // namespace tae
