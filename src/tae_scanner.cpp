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
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/storage/statistics/node_statistics.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/table_column.hpp"
#include "duckdb/parser/parsed_data/sample_options.hpp"
#include "duckdb/storage/table/row_group_reorderer.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/file_open_flags.hpp"
#include "duckdb/main/client_context.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <unordered_map>

// Minimal JSON parsing (header-only, bundled with DuckDB)
#include "yyjson.hpp"
using namespace duckdb_yyjson; // NOLINT

namespace tae {

// Decode hex string to bytes. Returns empty vector on invalid input.
static std::vector<uint8_t> HexDecode(const char *hex, size_t len) {
    if (len % 2 != 0) return {};
    std::vector<uint8_t> out(len / 2);
    for (size_t i = 0; i < len; i += 2) {
        auto hi = hex[i], lo = hex[i + 1];
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int h = nibble(hi), l = nibble(lo);
        if (h < 0 || l < 0) return {};
        out[i / 2] = static_cast<uint8_t>((h << 4) | l);
    }
    return out;
}

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
static void ParseManifest(duckdb::ClientContext &context,
                           const std::string &manifest_path,
                           TAEScanBindData &bind) {
    // Read manifest via DuckDB FileSystem (supports local files and HTTP URLs)
    auto &fs = duckdb::FileSystem::GetFileSystem(context);
    auto file_handle = fs.OpenFile(manifest_path,
                                   duckdb::FileFlags::FILE_FLAGS_READ);
    auto file_size = file_handle->GetFileSize();
    std::string json_str(file_size, '\0');
    file_handle->Read(const_cast<char *>(json_str.data()), file_size);
    file_handle->Close();

    yyjson_doc *doc = yyjson_read(json_str.c_str(), json_str.size(), 0);
    if (!doc) throw std::runtime_error("tae_scan: invalid JSON in manifest");

    yyjson_val *root = yyjson_doc_get_root(doc);

    // Database / table names
    yyjson_val *db_val = yyjson_obj_get(root, "database");
    yyjson_val *tbl_val = yyjson_obj_get(root, "table");
    if (db_val)  bind.db_name    = yyjson_get_str(db_val);
    if (tbl_val) bind.table_name = yyjson_get_str(tbl_val);

    // Data directory from manifest (where object files are stored)
    yyjson_val *dir_val = yyjson_obj_get(root, "data_dir");
    if (dir_val && yyjson_is_str(dir_val)) {
        bind.data_dir = yyjson_get_str(dir_val);
    }

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
        // Optional: hex-encoded 64-byte zone map for the sort key column
        auto *zm_val = yyjson_obj_get(obj_val, "zone_map");
        if (zm_val && yyjson_is_str(zm_val)) {
            auto zm_hex = yyjson_get_str(zm_val);
            auto zm_len = yyjson_get_len(zm_val);
            auto zm_bytes = HexDecode(zm_hex, zm_len);
            if (zm_bytes.size() == 64) {
                info.sort_key_zm = std::move(zm_bytes);
            }
        }
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

    // Parse the manifest JSON (supports local files and HTTP URLs)
    ParseManifest(context, manifest_path, *bind_data);

    // Use data_dir from manifest if present, otherwise derive from manifest location.
    // For HTTP URLs, data_dir MUST be in the manifest (no parent_path for URLs).
    if (bind_data->data_dir.empty()) {
        if (manifest_path.rfind("http://", 0) == 0 || manifest_path.rfind("https://", 0) == 0) {
            throw std::runtime_error("tae_scan: HTTP manifest must contain 'data_dir' field");
        }
        bind_data->data_dir = std::filesystem::path(manifest_path).parent_path().string();
    }

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
    // Phase 3: Extract pushed-down filters.
    // entry.GetIndex() is a position in column_ids; we map it to
    // the decoded_cols position via col_ids_to_decoded.
    if (input.filters) {
        for (auto &entry : input.filters->filters) {
            auto ci = static_cast<duckdb::idx_t>(entry.first);
            auto &filter = *entry.second;
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
    //
    // Fast path: if an object has a sort_key_zm (from manifest), check it first.
    // If the sort key zone map alone eliminates the object, skip without ReadMeta().
    if (!state->filters.empty()) {
        auto &fs = duckdb::FileSystem::GetFileSystem(context);
        uint16_t sort_seqnum = (bind.sort_column_idx >= 0)
            ? static_cast<uint16_t>(bind.sort_column_idx) : UINT16_MAX;

        for (uint32_t obj = 0; obj < bind.objects.size(); obj++) {
            auto &obj_info = bind.objects[obj];

            // Fast path: object-level sort key zone map from manifest
            if (!obj_info.sort_key_zm.empty() && sort_seqnum != UINT16_MAX) {
                if (!ZoneMapPassesFilters(state->filters, obj_info.sort_key_zm.data(),
                                           sort_seqnum)) {
                    // Sort key filter eliminates entire object — no metadata read needed
                    state->blocks_skipped.fetch_add(obj_info.blocks, std::memory_order_relaxed);
                    state->objects_skipped.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
            }

            // Slow path: read per-block metadata and check all filters
            bool any_block_passes = false;
            try {
                auto path = std::filesystem::path(bind.data_dir) / obj_info.file_path;
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
                for (uint32_t blk = 0; blk < obj_info.blocks; blk++) {
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
                    ZoneMap zmobj(zm);
                    if (order.use_min_stat) {
                        key = order.is_string
                            ? duckdb::Value(ZoneMapBytesToString(zmobj.MinBuf(), zmobj.MinLen()))
                            : ZoneMapBytesToValue(zmobj.MinBuf(), mo_oid, col_type);
                    } else {
                        key = order.is_string
                            ? duckdb::Value(ZoneMapBytesToString(zmobj.MaxBuf(), zmobj.MaxLen()))
                            : ZoneMapBytesToValue(zmobj.MaxBuf(), mo_oid, col_type);
                    }
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
                        if (a.key.IsNull()) return false;
                        if (b.key.IsNull()) return true;
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

// Compute per-block row count from manifest metadata (no file I/O).
// All blocks except the last have exactly 8192 rows.
static inline duckdb::idx_t ManifestBlockRowCount(
    const TAEObjectInfo &obj, uint32_t block_idx) {
    constexpr uint32_t ROWS_PER_BLOCK = 8192;
    if (block_idx + 1 < obj.blocks) return ROWS_PER_BLOCK;
    return obj.rows - static_cast<duckdb::idx_t>(obj.blocks - 1) * ROWS_PER_BLOCK;
}

// ===================================================================
// Execute — parallel scan with atomic work dispatch
//
// MO blocks have 8192 rows but DuckDB STANDARD_VECTOR_SIZE is 2048.
// We buffer decoded columns in local state and emit ≤2048 rows per call.
// ===================================================================
static void TAEScanExecute(duckdb::ClientContext &context,
                            duckdb::TableFunctionInput &input,
                            duckdb::DataChunk &output) {
    auto &bind = input.bind_data->Cast<TAEScanBindData>();
    auto &gstate = input.global_state->Cast<TAEScanState>();
    auto *lstate = input.local_state
                       ? &input.local_state->Cast<TAEScanLocalState>()
                       : nullptr;

    // Fast path: no TAE columns to read and no row filters.
    // Emit row counts from manifest metadata without any file I/O.
    // Covers count(*) and virtual-column-only queries.
    if (gstate.read_seqnums.empty() && gstate.filters.empty() && !gstate.do_sample) {
        // Drain pending rows from previous call
        if (lstate && lstate->pending_offset < lstate->pending_total_rows) {
            goto emit_chunk_fast;
        }
        while (true) {
            auto wu_idx = gstate.next_work_unit.fetch_add(1);
            if (wu_idx >= gstate.work_units.size()) {
                output.SetCardinality(0);
                return;
            }
            auto &wu = gstate.work_units[wu_idx];
            duckdb::idx_t total_rows = ManifestBlockRowCount(
                bind.objects[wu.object_idx], wu.block_idx);
            if (lstate) {
                lstate->pending_total_rows = total_rows;
                lstate->pending_offset = 0;
                lstate->pending_object_idx = wu.object_idx;
                lstate->pending_block_idx = wu.block_idx;
            } else {
                duckdb::idx_t chunk_rows = std::min(total_rows,
                    static_cast<duckdb::idx_t>(STANDARD_VECTOR_SIZE));
                output.SetCardinality(chunk_rows);
                gstate.rows_emitted.fetch_add(chunk_rows, std::memory_order_relaxed);
                return;
            }
            goto emit_chunk_fast;
        }

    emit_chunk_fast:
        {
            duckdb::idx_t src_offset = lstate->pending_offset;
            duckdb::idx_t remaining = lstate->pending_total_rows - src_offset;
            duckdb::idx_t chunk_rows = std::min(remaining,
                static_cast<duckdb::idx_t>(STANDARD_VECTOR_SIZE));
            output.SetCardinality(chunk_rows);
            // Fill virtual columns only
            for (duckdb::idx_t i = 0; i < gstate.output_map.size(); i++) {
                auto &om = gstate.output_map[i];
                if (om.kind == OutputColumnInfo::VCOL_FILENAME) {
                    auto fname = bind.objects[lstate->pending_object_idx].file_path;
                    auto target = duckdb::StringVector::AddString(output.data[i], fname);
                    auto *data = duckdb::FlatVector::GetData<duckdb::string_t>(output.data[i]);
                    std::fill_n(data, chunk_rows, target);
                } else if (om.kind == OutputColumnInfo::VCOL_BLOCK_ID) {
                    auto *data = duckdb::FlatVector::GetData<int32_t>(output.data[i]);
                    std::fill_n(data, chunk_rows, static_cast<int32_t>(lstate->pending_block_idx));
                }
            }
            lstate->pending_offset += chunk_rows;
            if (lstate->pending_offset >= lstate->pending_total_rows) {
                lstate->pending_total_rows = 0;
                lstate->pending_offset = 0;
            }
            gstate.rows_emitted.fetch_add(chunk_rows, std::memory_order_relaxed);
            return;
        }
    }

    // If we have pending rows from a previous block, emit the next chunk
    if (lstate && lstate->pending_offset < lstate->pending_total_rows) {
        goto emit_chunk;
    }

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

        // Look-ahead prefetch: hint the OS to start reading the next block
        auto peek_idx = gstate.next_work_unit.load(std::memory_order_relaxed);
        if (peek_idx < gstate.work_units.size()) {
            auto &next_wu = gstate.work_units[peek_idx];
            if (next_wu.object_idx == wu.object_idx) {
                reader->PrefetchBlock(next_wu.block_idx, gstate.read_seqnums);
            }
        }

        auto decoded_cols = reader->ReadBlock(wu.block_idx, gstate.read_seqnums);
        if (decoded_cols.empty() && !gstate.read_seqnums.empty()) continue;

        // Determine total row count
        duckdb::idx_t total_rows = 0;
        if (!decoded_cols.empty()) {
            total_rows = decoded_cols[0].row_count;
        } else {
            total_rows = reader->BlockRowCount(wu.block_idx);
        }

        // Store decoded block in local state for chunked emission
        if (lstate) {
            lstate->pending_cols = std::move(decoded_cols);
            lstate->pending_total_rows = total_rows;
            lstate->pending_offset = 0;
            lstate->pending_object_idx = wu.object_idx;
            lstate->pending_block_idx = wu.block_idx;
        } else {
            // No local state — emit first STANDARD_VECTOR_SIZE rows only
            duckdb::idx_t chunk_rows = std::min(total_rows,
                                                 static_cast<duckdb::idx_t>(STANDARD_VECTOR_SIZE));
            output.SetCardinality(chunk_rows);
            for (duckdb::idx_t i = 0; i < gstate.output_map.size(); i++) {
                auto &om = gstate.output_map[i];
                if (om.kind == OutputColumnInfo::TAE_COLUMN && om.decoded_pos < decoded_cols.size()) {
                    FillColumn(output.data[i], decoded_cols[om.decoded_pos], chunk_rows, 0);
                }
            }
            gstate.rows_emitted.fetch_add(chunk_rows, std::memory_order_relaxed);
            return;
        }

        // Fall through to emit_chunk
        goto emit_chunk;
    }

emit_chunk:
    {
        auto &pending_cols = lstate->pending_cols;
        duckdb::idx_t src_offset = lstate->pending_offset;
        duckdb::idx_t remaining = lstate->pending_total_rows - src_offset;
        duckdb::idx_t chunk_rows = std::min(remaining,
                                             static_cast<duckdb::idx_t>(STANDARD_VECTOR_SIZE));

        output.SetCardinality(chunk_rows);

        // Fill output columns
        for (duckdb::idx_t i = 0; i < gstate.output_map.size(); i++) {
            auto &om = gstate.output_map[i];
            switch (om.kind) {
            case OutputColumnInfo::TAE_COLUMN:
                if (om.decoded_pos < pending_cols.size()) {
                    FillColumn(output.data[i], pending_cols[om.decoded_pos],
                               chunk_rows, src_offset);
                }
                break;
            case OutputColumnInfo::VCOL_FILENAME: {
                auto fname = bind.objects[lstate->pending_object_idx].file_path;
                auto target = duckdb::StringVector::AddString(output.data[i], fname);
                auto *data = duckdb::FlatVector::GetData<duckdb::string_t>(output.data[i]);
                std::fill_n(data, chunk_rows, target);
                break;
            }
            case OutputColumnInfo::VCOL_BLOCK_ID: {
                auto *data = duckdb::FlatVector::GetData<int32_t>(output.data[i]);
                std::fill_n(data, chunk_rows, static_cast<int32_t>(lstate->pending_block_idx));
                break;
            }
            }
        }

        // Apply per-row filtering
        duckdb::idx_t filtered_count = ApplyRowFilters(
            gstate.filters, pending_cols, output, chunk_rows, src_offset);

        // Advance offset
        lstate->pending_offset += chunk_rows;
        if (lstate->pending_offset >= lstate->pending_total_rows) {
            // Block fully emitted — clear pending state
            lstate->pending_cols.clear();
            lstate->pending_total_rows = 0;
            lstate->pending_offset = 0;
        }

        if (filtered_count == 0) {
            // All rows in this chunk filtered — try next chunk or block
            if (lstate->pending_offset < lstate->pending_total_rows) {
                goto emit_chunk;
            }
            // Block exhausted, loop back to grab next work unit
            // Reset output and continue the main loop
            output.SetCardinality(0);
            // We can't easily loop back from here, so just return 0
            // and let DuckDB call us again
            return;
        }

        // Apply Bernoulli sampling if requested
        if (gstate.do_sample) {
            duckdb::SelectionVector sel(filtered_count);
            duckdb::idx_t sample_count = 0;
            for (duckdb::idx_t i = 0; i < filtered_count; i++) {
                if (lstate->dist(lstate->rng) <= gstate.sample_rate) {
                    sel.set_index(sample_count++, i);
                }
            }
            if (sample_count == 0) {
                if (lstate->pending_offset < lstate->pending_total_rows) {
                    goto emit_chunk;
                }
                output.SetCardinality(0);
                return;
            }
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

    // Skip expensive column statistics for large object counts.
    // Each object requires reading file metadata (costly for CRC-wrapped files).
    constexpr size_t MAX_OBJECTS_FOR_STATS = 16;
    if (bind.objects.size() > MAX_OBJECTS_FOR_STATS) {
        return nullptr;
    }

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
                ZoneMap zmobj(zm);

                if (IsStringType(oid)) {
                    auto min_val = duckdb::Value(ZoneMapBytesToString(zmobj.MinBuf(), zmobj.MinLen()));
                    auto max_val = duckdb::Value(ZoneMapBytesToString(zmobj.MaxBuf(), zmobj.MaxLen()));
                    if (!has_stats) {
                        global_min = std::move(min_val);
                        global_max = std::move(max_val);
                        has_stats = true;
                    } else {
                        if (min_val < global_min) global_min = std::move(min_val);
                        if (max_val > global_max) global_max = std::move(max_val);
                    }
                } else {
                    auto min_val = ZoneMapBytesToValue(zmobj.MinBuf(), oid, col_type);
                    auto max_val = ZoneMapBytesToValue(zmobj.MaxBuf(), oid, col_type);
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
        // Zone map min/max strings may be shorter than actual column values,
        // so don't advertise max_string_length (which Update sets from these).
        duckdb::StringStats::ResetMaxStringLength(stats);
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
