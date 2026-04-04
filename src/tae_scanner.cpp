// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0
//
// DuckDB TableFunction implementation for TAE scanner.
//
// Bind:    Parse manifest JSON, discover schema, extract pushed filters
// Init:    Create scan state, store projection + filter info
// Execute: Read blocks from TAE objects, evaluate zone maps, fill DataChunk

#include "tae_scanner.hpp"
#include "tae_column_fill.hpp"
#include "tae_filter.hpp"
#include "tae_types.hpp"

#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/planner/table_filter_set.hpp"
#include "duckdb/storage/statistics/node_statistics.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/table_column.hpp"
#include "duckdb/parser/parsed_data/sample_options.hpp"
#include "duckdb/storage/table/row_group_reorderer.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <unordered_map>

// Minimal JSON parsing (header-only, bundled with DuckDB)
#include "yyjson.hpp"
using namespace duckdb_yyjson; // NOLINT

namespace tae {

// ===================================================================
// Manifest JSON parsing
// ===================================================================

// Parse manifest JSON file produced by tae_manifest_gen.py
// Format:
//   {
//     "database": "tpch",
//     "table": "lineitem",
//     "columns": [
//       {"name":"l_orderkey", "oid":23, "width":8, "scale":0, "seqnum":0}, ...
//     ],
//     "objects": [
//       {"path":"018e.../00001", "rows":8192, "blocks":1, "size":131072}, ...
//     ]
//   }
static void ParseManifest(const std::string &manifest_path,
                           TAEScanBindData &bind) {
    // Read entire file
    std::ifstream ifs(manifest_path);
    if (!ifs.is_open()) {
        throw std::runtime_error("tae_scan: cannot open manifest: " + manifest_path);
    }
    std::string json_str((std::istreambuf_iterator<char>(ifs)),
                          std::istreambuf_iterator<char>());

    yyjson_doc *doc = yyjson_read(json_str.c_str(), json_str.size(), 0);
    if (!doc) throw std::runtime_error("tae_scan: invalid JSON in manifest");

    yyjson_val *root = yyjson_doc_get_root(doc);

    // Database / table names
    yyjson_val *db_val = yyjson_obj_get(root, "database");
    yyjson_val *tbl_val = yyjson_obj_get(root, "table");
    if (db_val)  bind.db_name    = yyjson_get_str(db_val);
    if (tbl_val) bind.table_name = yyjson_get_str(tbl_val);

    // Columns
    yyjson_val *cols = yyjson_obj_get(root, "columns");
    size_t col_idx, col_max;
    yyjson_val *col_val;
    yyjson_arr_foreach(cols, col_idx, col_max, col_val) {
        bind.all_col_names.push_back(yyjson_get_str(yyjson_obj_get(col_val, "name")));
        uint8_t oid = static_cast<uint8_t>(yyjson_get_int(yyjson_obj_get(col_val, "oid")));
        bind.all_col_mo_oids.push_back(oid);
        MOType mt = {};
        mt.oid = oid;
        auto *w = yyjson_obj_get(col_val, "width");
        auto *s = yyjson_obj_get(col_val, "scale");
        if (w) mt.width = static_cast<int32_t>(yyjson_get_int(w));
        if (s) mt.scale = static_cast<int32_t>(yyjson_get_int(s));
        bind.all_col_types.push_back(MOTypeToDuckDB(mt));
    }

    // Objects
    yyjson_val *objs = yyjson_obj_get(root, "objects");
    size_t obj_idx, obj_max;
    yyjson_val *obj_val;
    yyjson_arr_foreach(objs, obj_idx, obj_max, obj_val) {
        TAEObjectInfo info;
        info.file_path  = yyjson_get_str(yyjson_obj_get(obj_val, "path"));
        info.rows       = static_cast<uint32_t>(yyjson_get_int(yyjson_obj_get(obj_val, "rows")));
        info.blocks     = static_cast<uint32_t>(yyjson_get_int(yyjson_obj_get(obj_val, "blocks")));
        auto *sz = yyjson_obj_get(obj_val, "size");
        info.size_bytes = sz ? static_cast<uint32_t>(yyjson_get_int(sz)) : 0;
        bind.objects.push_back(std::move(info));
    }

    // Compute totals for cardinality/progress estimation
    bind.total_rows = 0;
    bind.total_blocks = 0;
    for (auto &o : bind.objects) {
        bind.total_rows += o.rows;
        bind.total_blocks += o.blocks;
    }

    // Parse optional sort_column (the PK or sort key column name)
    yyjson_val *sort_col = yyjson_obj_get(root, "sort_column");
    if (sort_col && yyjson_is_str(sort_col)) {
        std::string sort_name = yyjson_get_str(sort_col);
        for (size_t i = 0; i < bind.all_col_names.size(); i++) {
            if (bind.all_col_names[i] == sort_name) {
                bind.sort_column_idx = static_cast<int32_t>(i);
                break;
            }
        }
    }

    yyjson_doc_free(doc);
}

// ===================================================================
// SetScanOrder — receive ORDER BY pushdown from DuckDB optimizer
// ===================================================================
// Called by the RowGroupPruner optimizer when it detects ORDER BY ... LIMIT N.
// We extract the relevant fields and store them in bind_data for Init to use.
static void TAESetScanOrder(duckdb::unique_ptr<duckdb::RowGroupOrderOptions> options,
                             duckdb::optional_ptr<duckdb::FunctionData> bind_data_p) {
    if (!bind_data_p || !options) return;
    auto &bind = bind_data_p->Cast<TAEScanBindData>();

    auto info = std::make_unique<ScanOrderInfo>();
    info->column_idx = options->column_idx.GetPrimaryIndex();
    info->ascending = (options->order_type == duckdb::OrderType::ASCENDING);
    info->use_min_stat = (options->order_by == duckdb::OrderByStatistics::MIN);
    info->is_string = (options->column_type == duckdb::OrderByColumnType::STRING);
    info->row_limit = options->row_limit.IsValid() ? options->row_limit.GetIndex() : 0;

    bind.scan_order = std::move(info);
}

// ===================================================================
// Bind — parse manifest, set up projection, extract filters
// ===================================================================
static duckdb::unique_ptr<duckdb::FunctionData>
TAEScanBind(duckdb::ClientContext &context,
            duckdb::TableFunctionBindInput &input,
            duckdb::vector<duckdb::LogicalType> &return_types,
            duckdb::vector<duckdb::string> &names) {

    auto bind_data = duckdb::make_uniq<TAEScanBindData>();
    auto manifest_path = input.inputs[0].GetValue<std::string>();

    // Parse the manifest JSON
    ParseManifest(manifest_path, *bind_data);

    // Derive data_dir from manifest file location
    bind_data->data_dir = std::filesystem::path(manifest_path).parent_path().string();

    // Return all columns to DuckDB (projection pushdown will select a subset)
    for (size_t i = 0; i < bind_data->all_col_names.size(); i++) {
        names.push_back(bind_data->all_col_names[i]);
        return_types.push_back(bind_data->all_col_types[i]);
    }

    return bind_data;
}

// ===================================================================
// Init — set up scan state, resolve projection, extract pushed filters
// ===================================================================
static duckdb::unique_ptr<duckdb::GlobalTableFunctionState>
TAEScanInit(duckdb::ClientContext &context,
            duckdb::TableFunctionInitInput &input) {
    auto &bind = input.bind_data->Cast<TAEScanBindData>();
    auto state = duckdb::make_uniq<TAEScanState>();

    // Phase 1: Collect ALL TAE columns from column_ids into read_seqnums.
    // Build a mapping: column_ids position → decoded_cols position.
    // When filter_prune is active, column_ids includes filter-only columns
    // that don't appear in the output DataChunk.
    std::vector<duckdb::idx_t> col_ids_to_decoded(input.column_ids.size(), UINT64_MAX);
    for (size_t ci = 0; ci < input.column_ids.size(); ci++) {
        auto id = static_cast<duckdb::column_t>(input.column_ids[ci]);
        if (!duckdb::IsVirtualColumn(id)) {
            col_ids_to_decoded[ci] = state->read_seqnums.size();
            state->read_seqnums.push_back(static_cast<uint16_t>(id));
        }
    }

    // Phase 2: Build output_map from projection_ids (or column_ids if absent).
    // projection_ids maps output DataChunk slots → positions in column_ids.
    auto &proj_ids = input.projection_ids;
    bool has_proj = !proj_ids.empty() && proj_ids.size() < input.column_ids.size();
    auto n_out = has_proj ? proj_ids.size() : input.column_ids.size();
    for (size_t oi = 0; oi < n_out; oi++) {
        auto ci = has_proj ? proj_ids[oi] : oi;
        auto id = static_cast<duckdb::column_t>(input.column_ids[ci]);
        if (id == VCOL_FILENAME) {
            state->output_map.push_back({OutputColumnInfo::VCOL_FILENAME, 0, 0});
        } else if (id == VCOL_BLOCK_ID) {
            state->output_map.push_back({OutputColumnInfo::VCOL_BLOCK_ID, 0, 0});
        } else if (duckdb::IsVirtualColumn(id)) {
            state->output_map.push_back({OutputColumnInfo::VCOL_FILENAME, 0, 0});
        } else {
            state->output_map.push_back({OutputColumnInfo::TAE_COLUMN,
                                         static_cast<duckdb::idx_t>(id),
                                         col_ids_to_decoded[ci]});
        }
    }

    // Phase 3: Extract pushed-down filters.
    // entry.GetIndex() is a position in column_ids; we map it to
    // the decoded_cols position via col_ids_to_decoded.
    if (input.filters) {
        for (auto &entry : *input.filters) {
            auto ci = static_cast<duckdb::idx_t>(entry.GetIndex());
            auto &filter = entry.Filter();
            if (ci >= input.column_ids.size()) continue;

            auto id = static_cast<duckdb::column_t>(input.column_ids[ci]);
            if (duckdb::IsVirtualColumn(id)) continue;
            auto table_col = static_cast<duckdb::idx_t>(id);
            if (table_col >= bind.all_col_mo_oids.size()) continue;

            auto decoded_pos = col_ids_to_decoded[ci];
            if (decoded_pos == UINT64_MAX) continue;

            uint8_t mo_oid = bind.all_col_mo_oids[table_col];
            uint16_t seqnum = static_cast<uint16_t>(table_col);
            ExtractFilter(filter,
                          static_cast<uint16_t>(decoded_pos),
                          seqnum, mo_oid,
                          state->filters);
        }
    }

    // Sampling pushdown: if DuckDB provides sample_options, apply Bernoulli
    if (input.sample_options) {
        auto &opts = *input.sample_options;
        if (opts.is_percentage) {
            state->sample_rate = opts.sample_size.GetValue<double>() / 100.0;
        } else {
            // Row-count based: approximate as percentage of total rows
            auto requested = opts.sample_size.GetValue<int64_t>();
            state->sample_rate = bind.total_rows > 0
                ? static_cast<double>(requested) / static_cast<double>(bind.total_rows)
                : 1.0;
        }
        state->do_sample = state->sample_rate < 1.0;
    }

    // Build flattened work queue with object-level pruning.
    // When filters are present, read each object's metadata and check zone maps
    // for all blocks. Objects where ALL blocks fail filters are skipped entirely.
    if (!state->filters.empty()) {
        auto &fs = duckdb::FileSystem::GetFileSystem(context);
        for (uint32_t obj = 0; obj < bind.objects.size(); obj++) {
            bool any_block_passes = false;
            try {
                auto path = std::filesystem::path(bind.data_dir) /
                            bind.objects[obj].file_path;
                TAEObjectReader reader(fs, path.string());
                reader.ReadMeta();
                for (uint32_t blk = 0; blk < reader.BlockCount(); blk++) {
                    if (BlockPassesFilters(state->filters, reader, blk)) {
                        state->work_units.push_back({obj, blk});
                        any_block_passes = true;
                    } else {
                        state->blocks_skipped.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            } catch (...) {
                // Can't read metadata — include all blocks (conservative)
                for (uint32_t blk = 0; blk < bind.objects[obj].blocks; blk++) {
                    state->work_units.push_back({obj, blk});
                }
                any_block_passes = true;
            }
            if (!any_block_passes) {
                state->objects_skipped.fetch_add(1, std::memory_order_relaxed);
            }
        }
    } else {
        for (uint32_t obj = 0; obj < bind.objects.size(); obj++) {
            for (uint32_t blk = 0; blk < bind.objects[obj].blocks; blk++) {
                state->work_units.push_back({obj, blk});
            }
        }
    }

    // ORDER BY pushdown: sort work units by zone map of the order column
    if (bind.scan_order && !state->work_units.empty()) {
        auto &order = *bind.scan_order;
        if (order.column_idx < bind.all_col_mo_oids.size()) {
            uint16_t order_seqnum = static_cast<uint16_t>(order.column_idx);
            auto mo_oid = static_cast<MOTypeOid>(bind.all_col_mo_oids[order.column_idx]);
            auto &col_type = bind.all_col_types[order.column_idx];

            // Read zone map sort key for each work unit
            struct SortableUnit {
                size_t idx;
                duckdb::Value key;
            };
            std::vector<SortableUnit> sortable;
            sortable.reserve(state->work_units.size());

            // Cache readers per object to avoid re-opening
            std::unordered_map<uint32_t, std::unique_ptr<TAEObjectReader>> readers;

            for (size_t i = 0; i < state->work_units.size(); i++) {
                auto &wu = state->work_units[i];

                auto it = readers.find(wu.object_idx);
                if (it == readers.end()) {
                    auto &fs = duckdb::FileSystem::GetFileSystem(context);
                    auto path = std::filesystem::path(bind.data_dir) /
                                bind.objects[wu.object_idx].file_path;
                    auto reader = std::make_unique<TAEObjectReader>(fs, path.string());
                    reader->ReadMeta();
                    it = readers.emplace(wu.object_idx, std::move(reader)).first;
                }

                const uint8_t *zm = it->second->GetZoneMap(wu.block_idx, order_seqnum);
                duckdb::Value key;
                if (zm) {
                    const uint8_t *ptr = zm + (order.use_min_stat ? ZM_MIN_OFF : ZM_MAX_OFF);
                    key = order.is_string
                        ? duckdb::Value(ZoneMapBytesToString(ptr))
                        : ZoneMapBytesToValue(ptr, mo_oid, col_type);
                }
                sortable.push_back({i, std::move(key)});
            }

            // Sort: nulls go to the end regardless of direction
            if (order.ascending) {
                std::sort(sortable.begin(), sortable.end(),
                    [](const SortableUnit &a, const SortableUnit &b) {
                        if (a.key.IsNull()) return false;
                        if (b.key.IsNull()) return true;
                        return a.key < b.key;
                    });
            } else {
                std::sort(sortable.begin(), sortable.end(),
                    [](const SortableUnit &a, const SortableUnit &b) {
                        if (a.key.IsNull()) return true;
                        if (b.key.IsNull()) return false;
                        return a.key > b.key;
                    });
            }

            // Reorder work_units
            auto old_units = std::move(state->work_units);
            state->work_units.reserve(sortable.size());
            for (auto &su : sortable) {
                state->work_units.push_back(old_units[su.idx]);
            }

            // Truncate if row_limit allows early termination
            if (order.row_limit > 0) {
                duckdb::idx_t rows_available = 0;
                for (size_t i = 0; i < state->work_units.size(); i++) {
                    auto &wu = state->work_units[i];
                    auto it = readers.find(wu.object_idx);
                    rows_available += it->second->BlockRowCount(wu.block_idx);
                    if (rows_available >= order.row_limit) {
                        state->work_units.resize(i + 1);
                        break;
                    }
                }
            }
        }
    }

    return state;
}

// ===================================================================
// Init local — per-thread reader and state
// ===================================================================
static duckdb::unique_ptr<duckdb::LocalTableFunctionState>
TAEScanInitLocal(duckdb::ExecutionContext &context,
                 duckdb::TableFunctionInitInput &input,
                 duckdb::GlobalTableFunctionState *global_state) {
    return duckdb::make_uniq<TAEScanLocalState>();
}

// ===================================================================
// Execute — parallel scan with atomic work dispatch
// ===================================================================
static void TAEScanExecute(duckdb::ClientContext &context,
                            duckdb::TableFunctionInput &input,
                            duckdb::DataChunk &output) {
    auto &bind = input.bind_data->Cast<TAEScanBindData>();
    auto &gstate = input.global_state->Cast<TAEScanState>();
    auto *lstate = input.local_state
                       ? &input.local_state->Cast<TAEScanLocalState>()
                       : nullptr;

    while (true) {
        // Atomically grab next work unit
        auto wu_idx = gstate.next_work_unit.fetch_add(1);
        if (wu_idx >= gstate.work_units.size()) {
            output.SetCardinality(0);
            return;
        }

        auto &wu = gstate.work_units[wu_idx];

        // Open reader for this object (reuse if same object)
        TAEObjectReader *reader = nullptr;
        std::unique_ptr<TAEObjectReader> temp_reader;

        if (lstate && lstate->reader_object_idx == wu.object_idx) {
            reader = lstate->reader.get();
        } else {
            auto &fs = duckdb::FileSystem::GetFileSystem(context);
            auto path = std::filesystem::path(bind.data_dir) /
                        bind.objects[wu.object_idx].file_path;
            auto new_reader = std::make_unique<TAEObjectReader>(fs, path.string());
            new_reader->ReadMeta();
            if (lstate) {
                lstate->reader = std::move(new_reader);
                lstate->reader_object_idx = wu.object_idx;
                reader = lstate->reader.get();
            } else {
                temp_reader = std::move(new_reader);
                reader = temp_reader.get();
            }
        }

        // Zone map filter: skip block if any filter rejects
        if (!gstate.filters.empty() &&
            !BlockPassesFilters(gstate.filters, *reader, wu.block_idx)) {
            gstate.blocks_skipped.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        // Read the block (only TAE columns, not virtual)
        gstate.blocks_scanned.fetch_add(1, std::memory_order_relaxed);
        auto decoded_cols = reader->ReadBlock(wu.block_idx, gstate.read_seqnums);
        if (decoded_cols.empty() && !gstate.read_seqnums.empty()) continue;

        // Determine row count from first decoded TAE column, or from object metadata
        duckdb::idx_t row_count = 0;
        if (!decoded_cols.empty()) {
            row_count = decoded_cols[0].row_count;
        } else {
            // All output columns are virtual — use block row count from metadata
            row_count = reader->BlockRowCount(wu.block_idx);
        }
        output.SetCardinality(row_count);

        // Fill output columns using output_map (decoded_pos indexes decoded_cols)
        for (duckdb::idx_t i = 0; i < gstate.output_map.size(); i++) {
            auto &om = gstate.output_map[i];
            switch (om.kind) {
            case OutputColumnInfo::TAE_COLUMN:
                if (om.decoded_pos < decoded_cols.size()) {
                    FillColumn(output.data[i], decoded_cols[om.decoded_pos], row_count);
                }
                break;
            case OutputColumnInfo::VCOL_FILENAME: {
                auto fname = bind.objects[wu.object_idx].file_path;
                auto target = duckdb::StringVector::AddString(output.data[i], fname);
                auto *data = duckdb::FlatVector::GetData<duckdb::string_t>(output.data[i]);
                std::fill_n(data, row_count, target);
                break;
            }
            case OutputColumnInfo::VCOL_BLOCK_ID: {
                auto *data = duckdb::FlatVector::GetData<int32_t>(output.data[i]);
                std::fill_n(data, row_count, static_cast<int32_t>(wu.block_idx));
                break;
            }
            }
        }

        // Apply per-row filtering (zone maps only do block-level skip)
        duckdb::idx_t filtered_count = ApplyRowFilters(
            gstate.filters, decoded_cols, output, row_count);
        if (filtered_count == 0) continue; // all rows filtered out

        // Apply Bernoulli sampling if requested
        if (gstate.do_sample && lstate) {
            duckdb::SelectionVector sel(filtered_count);
            duckdb::idx_t sample_count = 0;
            for (duckdb::idx_t i = 0; i < filtered_count; i++) {
                if (lstate->dist(lstate->rng) <= gstate.sample_rate) {
                    sel.set_index(sample_count++, i);
                }
            }
            if (sample_count == 0) continue;
            output.Slice(sel, sample_count);
            filtered_count = sample_count;
        }

        gstate.rows_emitted.fetch_add(filtered_count, std::memory_order_relaxed);
        return;
    }
}

// ===================================================================
// Cardinality — provide row-count estimate from manifest metadata
// ===================================================================
static duckdb::unique_ptr<duckdb::NodeStatistics>
TAEScanCardinality(duckdb::ClientContext &context,
                   const duckdb::FunctionData *bind_data_p) {
    if (!bind_data_p) return nullptr;
    auto &bind = bind_data_p->Cast<TAEScanBindData>();
    return duckdb::make_uniq<duckdb::NodeStatistics>(bind.total_rows, bind.total_rows);
}

// ===================================================================
// Statistics — provide column-level min/max from zone maps
// ===================================================================

static duckdb::unique_ptr<duckdb::BaseStatistics>
TAEScanStatistics(duckdb::ClientContext &context,
                  const duckdb::FunctionData *bind_data_p,
                  duckdb::column_t column_index) {
    if (!bind_data_p) return nullptr;
    auto &bind = bind_data_p->Cast<TAEScanBindData>();

    if (column_index >= bind.all_col_types.size()) return nullptr;

    auto &col_type = bind.all_col_types[column_index];
    auto oid = static_cast<MOTypeOid>(bind.all_col_mo_oids[column_index]);
    uint16_t seqnum = static_cast<uint16_t>(column_index);

    // Open each object and merge zone maps for this column
    bool has_stats = false;
    duckdb::Value global_min, global_max;
    bool has_nulls = false;

    for (auto &obj : bind.objects) {
        std::string full_path = bind.data_dir + "/" + obj.file_path;
        try {
            auto &fs = duckdb::FileSystem::GetFileSystem(context);
            TAEObjectReader reader(fs, full_path);
            reader.ReadMeta();

            for (uint32_t blk = 0; blk < reader.BlockCount(); blk++) {
                const uint8_t *zm = reader.GetZoneMap(blk, seqnum);
                if (!zm) continue;

                // zone map layout: min at offset 0, max at offset ZM_MAX_OFF (31)
                const uint8_t *zm_min = zm + ZM_MIN_OFF;
                const uint8_t *zm_max = zm + ZM_MAX_OFF;

                if (IsStringType(oid)) {
                    auto min_val = duckdb::Value(ZoneMapBytesToString(zm_min));
                    auto max_val = duckdb::Value(ZoneMapBytesToString(zm_max));
                    if (!has_stats) {
                        global_min = std::move(min_val);
                        global_max = std::move(max_val);
                        has_stats = true;
                    } else {
                        if (min_val < global_min) global_min = std::move(min_val);
                        if (max_val > global_max) global_max = std::move(max_val);
                    }
                } else {
                    auto min_val = ZoneMapBytesToValue(zm_min, oid, col_type);
                    auto max_val = ZoneMapBytesToValue(zm_max, oid, col_type);
                    if (min_val.IsNull() || max_val.IsNull()) continue;
                    if (!has_stats) {
                        global_min = std::move(min_val);
                        global_max = std::move(max_val);
                        has_stats = true;
                    } else {
                        if (min_val < global_min) global_min = std::move(min_val);
                        if (max_val > global_max) global_max = std::move(max_val);
                    }
                }

                // Check null count from column metadata
                auto &blk_info = reader.Meta().blocks[blk];
                if (seqnum < blk_info.columns.size() &&
                    blk_info.columns[seqnum].null_cnt > 0) {
                    has_nulls = true;
                }
            }
        } catch (...) {
            return nullptr; // can't read → no stats
        }
    }

    if (!has_stats) return nullptr;

    auto stats = duckdb::BaseStatistics::CreateEmpty(col_type);
    if (has_nulls) stats.SetHasNull();
    if (!has_nulls) stats.SetHasNoNull();

    switch (col_type.InternalType()) {
    case duckdb::PhysicalType::INT8:
    case duckdb::PhysicalType::INT16:
    case duckdb::PhysicalType::INT32:
    case duckdb::PhysicalType::INT64:
    case duckdb::PhysicalType::UINT8:
    case duckdb::PhysicalType::UINT16:
    case duckdb::PhysicalType::UINT32:
    case duckdb::PhysicalType::UINT64:
    case duckdb::PhysicalType::FLOAT:
    case duckdb::PhysicalType::DOUBLE:
    case duckdb::PhysicalType::BOOL:
    case duckdb::PhysicalType::INT128:
        duckdb::NumericStats::SetMin(stats, global_min);
        duckdb::NumericStats::SetMax(stats, global_max);
        break;
    case duckdb::PhysicalType::VARCHAR: {
        auto min_sv = duckdb::StringValue::Get(global_min);
        auto max_sv = duckdb::StringValue::Get(global_max);
        duckdb::StringStats::Update(stats, min_sv);
        duckdb::StringStats::Update(stats, max_sv);
        break;
    }
    default:
        return nullptr;
    }

    return duckdb::make_uniq<duckdb::BaseStatistics>(std::move(stats));
}

// ===================================================================
// Progress — scan completion percentage (0–100)
// ===================================================================
static double TAEScanProgress(duckdb::ClientContext &context,
                              const duckdb::FunctionData *bind_data_p,
                              const duckdb::GlobalTableFunctionState *global_state) {
    if (!bind_data_p || !global_state) return -1.0;
    auto &bind = bind_data_p->Cast<TAEScanBindData>();
    auto &state = global_state->Cast<TAEScanState>();

    if (bind.total_blocks == 0) return 100.0;
    double done = static_cast<double>(
        state.blocks_scanned.load(std::memory_order_relaxed) +
        state.blocks_skipped.load(std::memory_order_relaxed));
    return (done / static_cast<double>(bind.total_blocks)) * 100.0;
}

// ===================================================================
// ToString — static info for EXPLAIN output
// ===================================================================
static duckdb::InsertionOrderPreservingMap<duckdb::string>
TAEScanToString(duckdb::TableFunctionToStringInput &input) {
    duckdb::InsertionOrderPreservingMap<duckdb::string> result;
    if (!input.bind_data) return result;
    auto &bind = input.bind_data->Cast<TAEScanBindData>();

    if (!bind.table_name.empty()) {
        result["Table"] = bind.db_name.empty() ? bind.table_name
                                                : bind.db_name + "." + bind.table_name;
    }
    result["Objects"] = std::to_string(bind.objects.size());
    result["Total Rows"] = std::to_string(bind.total_rows);
    result["Total Blocks"] = std::to_string(bind.total_blocks);
    return result;
}

// ===================================================================
// DynamicToString — runtime info for profiling
// ===================================================================
static duckdb::InsertionOrderPreservingMap<duckdb::string>
TAEScanDynamicToString(duckdb::TableFunctionDynamicToStringInput &input) {
    duckdb::InsertionOrderPreservingMap<duckdb::string> result;
    if (!input.global_state) return result;
    auto &state = input.global_state->Cast<TAEScanState>();

    result["Blocks Scanned"] = std::to_string(state.blocks_scanned.load(std::memory_order_relaxed));
    result["Blocks Skipped"] = std::to_string(state.blocks_skipped.load(std::memory_order_relaxed));
    auto obj_skip = state.objects_skipped.load(std::memory_order_relaxed);
    if (obj_skip > 0) {
        result["Objects Skipped"] = std::to_string(obj_skip);
    }
    result["Rows Emitted"] = std::to_string(state.rows_emitted.load(std::memory_order_relaxed));
    return result;
}

// ===================================================================
// RowsScanned — actual row count for profiling
// ===================================================================
static duckdb::idx_t TAEScanRowsScanned(duckdb::GlobalTableFunctionState &global_state,
                                         duckdb::LocalTableFunctionState &local_state) {
    auto &state = global_state.Cast<TAEScanState>();
    return state.rows_emitted.load(std::memory_order_relaxed);
}

// ===================================================================
// GetVirtualColumns — expose file_path and block_id virtual columns
// ===================================================================
static duckdb::virtual_column_map_t
TAEScanGetVirtualColumns(duckdb::ClientContext &context,
                         duckdb::optional_ptr<duckdb::FunctionData> bind_data) {
    duckdb::virtual_column_map_t result;
    result.emplace(VCOL_FILENAME, duckdb::TableColumn("file_path", duckdb::LogicalType::VARCHAR));
    result.emplace(VCOL_BLOCK_ID, duckdb::TableColumn("block_id", duckdb::LogicalType::INTEGER));
    return result;
}

// ===================================================================
// SupportsPushdownType — which column types support filter pushdown
// ===================================================================
static bool TAEScanSupportsPushdownType(const duckdb::FunctionData &bind_data_p,
                                        duckdb::idx_t col_idx) {
    auto &bind = bind_data_p.Cast<TAEScanBindData>();
    if (col_idx >= bind.all_col_types.size()) return false;
    auto &type = bind.all_col_types[col_idx];
    switch (type.id()) {
    case duckdb::LogicalTypeId::TINYINT:
    case duckdb::LogicalTypeId::SMALLINT:
    case duckdb::LogicalTypeId::INTEGER:
    case duckdb::LogicalTypeId::BIGINT:
    case duckdb::LogicalTypeId::UTINYINT:
    case duckdb::LogicalTypeId::USMALLINT:
    case duckdb::LogicalTypeId::UINTEGER:
    case duckdb::LogicalTypeId::UBIGINT:
    case duckdb::LogicalTypeId::FLOAT:
    case duckdb::LogicalTypeId::DOUBLE:
    case duckdb::LogicalTypeId::BOOLEAN:
    case duckdb::LogicalTypeId::DATE:
    case duckdb::LogicalTypeId::TIMESTAMP:
    case duckdb::LogicalTypeId::VARCHAR:
        return true;
    default:
        return false;
    }
}

// ===================================================================
// GetTAEScanFunction — construct the TableFunction with filter pushdown
// ===================================================================
duckdb::TableFunction GetTAEScanFunction() {
    duckdb::TableFunction func("tae_scan",
                                {duckdb::LogicalType::VARCHAR},  // manifest path
                                TAEScanExecute);
    func.bind = TAEScanBind;
    func.init_global = TAEScanInit;
    func.init_local = TAEScanInitLocal;
    func.projection_pushdown = true;
    func.filter_pushdown = true;
    func.filter_prune = true;
    func.cardinality = TAEScanCardinality;
    func.statistics = TAEScanStatistics;
    func.table_scan_progress = TAEScanProgress;
    func.to_string = TAEScanToString;
    func.dynamic_to_string = TAEScanDynamicToString;
    func.rows_scanned = TAEScanRowsScanned;
    func.get_virtual_columns = TAEScanGetVirtualColumns;
    func.supports_pushdown_type = TAEScanSupportsPushdownType;
    func.sampling_pushdown = true;
    func.set_scan_order = TAESetScanOrder;

    return func;
}

} // namespace tae
