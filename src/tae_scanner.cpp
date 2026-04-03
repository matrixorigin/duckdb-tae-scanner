// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0
//
// DuckDB TableFunction implementation for TAE scanner.
//
// Bind:    Parse manifest JSON, discover schema, extract pushed filters
// Init:    Create scan state, store projection + filter info
// Execute: Read blocks from TAE objects, evaluate zone maps, fill DataChunk

#include "tae_scanner.hpp"
#include "tae_types.hpp"

#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/null_filter.hpp"
#include "duckdb/planner/table_filter_set.hpp"

#include <cstring>
#include <fstream>
#include <filesystem>

// Minimal JSON parsing (header-only, bundled with DuckDB)
#include "yyjson.hpp"
using namespace duckdb_yyjson; // NOLINT

namespace tae {

// ===================================================================
// Column-fill helpers: copy decoded TAE columns into DuckDB vectors
// ===================================================================

static void CopyFixedColumn(duckdb::Vector &out_vec,
                             const DecodedColumn &col,
                             duckdb::idx_t count) {
    auto elem_size = MOTypeFixedSize(static_cast<MOTypeOid>(col.type.oid));
    if (elem_size <= 0) return;
    auto *dst = duckdb::FlatVector::GetData(out_vec);
    memcpy(dst, col.data.data(), count * static_cast<size_t>(elem_size));
}

static void CopyDateColumn(duckdb::Vector &out_vec,
                            const DecodedColumn &col,
                            duckdb::idx_t count) {
    auto *src = reinterpret_cast<const int32_t *>(col.data.data());
    auto *dst = duckdb::FlatVector::GetData<int32_t>(out_vec);
    for (duckdb::idx_t i = 0; i < count; i++) {
        dst[i] = src[i] - MO_UNIX_EPOCH_DAYS;
    }
}

static void CopyTimestampColumn(duckdb::Vector &out_vec,
                                 const DecodedColumn &col,
                                 duckdb::idx_t count) {
    auto *src = reinterpret_cast<const int64_t *>(col.data.data());
    auto *dst = duckdb::FlatVector::GetData<int64_t>(out_vec);
    for (duckdb::idx_t i = 0; i < count; i++) {
        dst[i] = src[i] - MO_UNIX_EPOCH_USEC;
    }
}

static void CopyVarlenColumn(duckdb::Vector &out_vec,
                              const DecodedColumn &col,
                              duckdb::idx_t count) {
    auto *src = reinterpret_cast<const Varlena *>(col.data.data());
    const uint8_t *area = col.area.data();

    for (duckdb::idx_t i = 0; i < count; i++) {
        const auto &v = src[i];
        const char *str_data;
        uint32_t str_len;

        if (v.is_inline()) {
            str_len = v.inline_length();
            str_data = v.inline_data();
        } else {
            uint32_t big_marker;
            memcpy(&big_marker, v.data, 4);
            if (big_marker != VARLENA_BIG_MARKER) {
                str_data = "";
                str_len = 0;
            } else {
                str_data = reinterpret_cast<const char *>(area + v.big_offset());
                str_len = v.big_length();
            }
        }

        auto target = duckdb::StringVector::AddString(out_vec, str_data, str_len);
        duckdb::FlatVector::GetData<duckdb::string_t>(out_vec)[i] = target;
    }
}

static void SetNullMask(duckdb::Vector &out_vec,
                         const DecodedColumn &col,
                         duckdb::idx_t count) {
    if (col.null_count == 0) return;
    auto &validity = duckdb::FlatVector::Validity(out_vec);
    for (duckdb::idx_t i = 0; i < count; i++) {
        uint64_t word_idx = i / 64;
        uint64_t bit_idx = i % 64;
        if (word_idx < col.null_bitmap.size() &&
            (col.null_bitmap[word_idx] & (1ULL << bit_idx))) {
            validity.SetInvalid(i);
        }
    }
}

static void FillColumn(duckdb::Vector &out_vec,
                        const DecodedColumn &col,
                        duckdb::idx_t count) {
    switch (static_cast<MOTypeOid>(col.type.oid)) {
    case MO_T_date:
        CopyDateColumn(out_vec, col, count);
        break;
    case MO_T_datetime:
    case MO_T_timestamp:
        CopyTimestampColumn(out_vec, col, count);
        break;
    case MO_T_char:
    case MO_T_varchar:
    case MO_T_blob:
    case MO_T_text:
    case MO_T_json:
    case MO_T_binary:
    case MO_T_varbinary:
    case MO_T_datalink:
        CopyVarlenColumn(out_vec, col, count);
        break;
    default:
        CopyFixedColumn(out_vec, col, count);
        break;
    }
    SetNullMask(out_vec, col, count);
}

// ===================================================================
// Filter conversion helpers
// ===================================================================

static bool IsStringType(uint8_t oid) {
    switch (static_cast<MOTypeOid>(oid)) {
    case MO_T_char: case MO_T_varchar: case MO_T_text:
    case MO_T_json: case MO_T_datalink:
    case MO_T_blob: case MO_T_binary: case MO_T_varbinary:
        return true;
    default:
        return false;
    }
}

// Convert DuckDB ExpressionType to our FilterOp
static bool ConvertExprType(duckdb::ExpressionType etype, FilterOp &out) {
    using ET = duckdb::ExpressionType;
    switch (etype) {
    case ET::COMPARE_EQUAL:                out = FilterOp::EQUAL;                  return true;
    case ET::COMPARE_NOTEQUAL:             out = FilterOp::NOT_EQUAL;              return true;
    case ET::COMPARE_GREATERTHAN:          out = FilterOp::GREATER_THAN;           return true;
    case ET::COMPARE_GREATERTHANOREQUALTO: out = FilterOp::GREATER_THAN_OR_EQUAL;  return true;
    case ET::COMPARE_LESSTHAN:             out = FilterOp::LESS_THAN;              return true;
    case ET::COMPARE_LESSTHANOREQUALTO:    out = FilterOp::LESS_THAN_OR_EQUAL;     return true;
    default: return false;
    }
}

// Encode a DuckDB Value as raw little-endian bytes (zone map encoding)
static bool EncodeConstant(const duckdb::Value &val, uint8_t mo_oid,
                            std::vector<uint8_t> &out_bytes, uint32_t &out_len) {
    if (IsStringType(mo_oid)) {
        auto s = val.GetValue<std::string>();
        out_bytes.assign(s.begin(), s.end());
        out_len = static_cast<uint32_t>(s.size());
        return true;
    }
    // Fixed-width: encode as raw bytes
    auto sz = MOTypeFixedSize(static_cast<MOTypeOid>(mo_oid));
    if (sz <= 0) return false;
    out_bytes.resize(static_cast<size_t>(sz));
    out_len = static_cast<uint32_t>(sz);
    switch (static_cast<MOTypeOid>(mo_oid)) {
    case MO_T_bool:    { auto v = val.GetValue<bool>();     memcpy(out_bytes.data(), &v, 1); break; }
    case MO_T_int8:    { auto v = val.GetValue<int8_t>();   memcpy(out_bytes.data(), &v, 1); break; }
    case MO_T_int16:   { auto v = val.GetValue<int16_t>();  memcpy(out_bytes.data(), &v, 2); break; }
    case MO_T_int32:   { auto v = val.GetValue<int32_t>();  memcpy(out_bytes.data(), &v, 4); break; }
    case MO_T_int64:   { auto v = val.GetValue<int64_t>();  memcpy(out_bytes.data(), &v, 8); break; }
    case MO_T_uint8:   { auto v = val.GetValue<uint8_t>();  memcpy(out_bytes.data(), &v, 1); break; }
    case MO_T_uint16:  { auto v = val.GetValue<uint16_t>(); memcpy(out_bytes.data(), &v, 2); break; }
    case MO_T_uint32:  { auto v = val.GetValue<uint32_t>(); memcpy(out_bytes.data(), &v, 4); break; }
    case MO_T_uint64:  { auto v = val.GetValue<uint64_t>(); memcpy(out_bytes.data(), &v, 8); break; }
    case MO_T_float32: { auto v = val.GetValue<float>();    memcpy(out_bytes.data(), &v, 4); break; }
    case MO_T_float64: { auto v = val.GetValue<double>();   memcpy(out_bytes.data(), &v, 8); break; }
    case MO_T_date: {
        // DuckDB date → MO date (add epoch offset)
        auto v = val.GetValue<int32_t>() + MO_UNIX_EPOCH_DAYS;
        memcpy(out_bytes.data(), &v, 4);
        break;
    }
    case MO_T_datetime:
    case MO_T_timestamp: {
        // DuckDB timestamp → MO timestamp (add epoch offset)
        auto v = val.GetValue<int64_t>() + MO_UNIX_EPOCH_USEC;
        memcpy(out_bytes.data(), &v, 8);
        break;
    }
    default: return false;
    }
    return true;
}

// Extract a single DuckDB TableFilter into PushedFilter(s)
static void ExtractFilter(const duckdb::TableFilter &filter,
                           uint16_t col_idx, uint16_t seqnum, uint8_t mo_oid,
                           std::vector<PushedFilter> &out) {
    switch (filter.filter_type) {
    case duckdb::TableFilterType::CONSTANT_COMPARISON: {
        auto &cf = filter.Cast<duckdb::ConstantFilter>();
        FilterOp op;
        if (!ConvertExprType(cf.comparison_type, op)) return;
        PushedFilter pf;
        pf.col_idx = col_idx;
        pf.seqnum = seqnum;
        pf.mo_type_oid = mo_oid;
        pf.op = op;
        if (!EncodeConstant(cf.constant, mo_oid, pf.constant, pf.const_len)) return;
        out.push_back(std::move(pf));
        break;
    }
    case duckdb::TableFilterType::IS_NULL: {
        PushedFilter pf;
        pf.col_idx = col_idx;
        pf.seqnum = seqnum;
        pf.mo_type_oid = mo_oid;
        pf.op = FilterOp::IS_NULL;
        pf.const_len = 0;
        out.push_back(std::move(pf));
        break;
    }
    case duckdb::TableFilterType::IS_NOT_NULL: {
        PushedFilter pf;
        pf.col_idx = col_idx;
        pf.seqnum = seqnum;
        pf.mo_type_oid = mo_oid;
        pf.op = FilterOp::IS_NOT_NULL;
        pf.const_len = 0;
        out.push_back(std::move(pf));
        break;
    }
    case duckdb::TableFilterType::CONJUNCTION_AND: {
        auto &cf = filter.Cast<duckdb::ConjunctionAndFilter>();
        for (auto &child : cf.child_filters) {
            ExtractFilter(*child, col_idx, seqnum, mo_oid, out);
        }
        break;
    }
    default:
        // Unsupported filter type — don't push down, DuckDB will still
        // evaluate it after we return the rows.
        break;
    }
}

// ===================================================================
// Zone map evaluation
// ===================================================================

// Evaluate one PushedFilter against a block's zone map.
// Returns true if block MIGHT match (keep it), false to skip.
static bool EvalFilterOnZoneMap(const PushedFilter &pf,
                                 const uint8_t *zm_data) {
    if (!zm_data) return true; // no zone map → can't skip

    ZoneMap zm(zm_data);
    if (!zm.IsInited()) return true;

    if (pf.op == FilterOp::IS_NULL || pf.op == FilterOp::IS_NOT_NULL) {
        return true; // zone maps don't track nulls per block
    }

    if (IsStringType(pf.mo_type_oid)) {
        return ZoneMapCheckString(zm, pf.op,
                                   reinterpret_cast<const char *>(pf.constant.data()),
                                   pf.const_len);
    }

    // Dispatch fixed-width comparison by MO type
    switch (static_cast<MOTypeOid>(pf.mo_type_oid)) {
    case MO_T_bool:    { bool v;     memcpy(&v, pf.constant.data(), 1); return ZoneMapCheckFixed(zm, pf.op, v); }
    case MO_T_int8:    { int8_t v;   memcpy(&v, pf.constant.data(), 1); return ZoneMapCheckFixed(zm, pf.op, v); }
    case MO_T_int16:   { int16_t v;  memcpy(&v, pf.constant.data(), 2); return ZoneMapCheckFixed(zm, pf.op, v); }
    case MO_T_int32:
    case MO_T_date:    { int32_t v;  memcpy(&v, pf.constant.data(), 4); return ZoneMapCheckFixed(zm, pf.op, v); }
    case MO_T_int64:
    case MO_T_datetime:
    case MO_T_timestamp: { int64_t v; memcpy(&v, pf.constant.data(), 8); return ZoneMapCheckFixed(zm, pf.op, v); }
    case MO_T_uint8:   { uint8_t v;  memcpy(&v, pf.constant.data(), 1); return ZoneMapCheckFixed(zm, pf.op, v); }
    case MO_T_uint16:  { uint16_t v; memcpy(&v, pf.constant.data(), 2); return ZoneMapCheckFixed(zm, pf.op, v); }
    case MO_T_uint32:  { uint32_t v; memcpy(&v, pf.constant.data(), 4); return ZoneMapCheckFixed(zm, pf.op, v); }
    case MO_T_uint64:  { uint64_t v; memcpy(&v, pf.constant.data(), 8); return ZoneMapCheckFixed(zm, pf.op, v); }
    case MO_T_float32: { float v;    memcpy(&v, pf.constant.data(), 4); return ZoneMapCheckFixed(zm, pf.op, v); }
    case MO_T_float64: { double v;   memcpy(&v, pf.constant.data(), 8); return ZoneMapCheckFixed(zm, pf.op, v); }
    default:
        return true; // unsupported type for zone map → keep block
    }
}

// Check all pushed filters against a block; skip if ANY filter rejects.
static bool BlockPassesFilters(const std::vector<PushedFilter> &filters,
                                const TAEObjectReader &reader,
                                uint32_t block_idx) {
    for (auto &f : filters) {
        const uint8_t *zm = reader.GetZoneMap(block_idx, f.seqnum);
        if (!EvalFilterOnZoneMap(f, zm)) {
            return false;
        }
    }
    return true;
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

    yyjson_doc_free(doc);
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

    return std::move(bind_data);
}

// ===================================================================
// Init — set up scan state, resolve projection, extract pushed filters
// ===================================================================
static duckdb::unique_ptr<duckdb::GlobalTableFunctionState>
TAEScanInit(duckdb::ClientContext &context,
            duckdb::TableFunctionInitInput &input) {
    auto &bind = input.bind_data->Cast<TAEScanBindData>();
    auto state = duckdb::make_uniq<TAEScanState>();

    // Resolve projected columns → TAE seqnums
    for (auto &col_id : input.column_ids) {
        auto idx = static_cast<duckdb::idx_t>(col_id);
        state->projected_col_indices.push_back(idx);
        state->read_seqnums.push_back(static_cast<uint16_t>(idx));
    }

    // Extract pushed-down filters from DuckDB's TableFilterSet
    if (input.filters) {
        for (auto &entry : *input.filters) {
            auto proj_idx = static_cast<duckdb::idx_t>(entry.GetIndex());
            auto &filter = entry.Filter();

            // Map projection index → table column index
            if (proj_idx >= state->projected_col_indices.size()) continue;
            auto table_col = state->projected_col_indices[proj_idx];
            if (table_col >= bind.all_col_mo_oids.size()) continue;

            uint8_t mo_oid = bind.all_col_mo_oids[table_col];
            uint16_t seqnum = static_cast<uint16_t>(table_col);

            ExtractFilter(filter,
                          static_cast<uint16_t>(proj_idx),
                          seqnum, mo_oid,
                          state->filters);
        }
    }

    return std::move(state);
}

// ===================================================================
// Execute — scan loop with zone map block skipping
// ===================================================================
static void TAEScanExecute(duckdb::ClientContext &context,
                            duckdb::TableFunctionInput &input,
                            duckdb::DataChunk &output) {
    auto &bind = input.bind_data->Cast<TAEScanBindData>();
    auto &state = input.global_state->Cast<TAEScanState>();

    while (state.current_object < bind.objects.size()) {
        // Open next object if needed
        if (!state.reader) {
            auto &fs = duckdb::FileSystem::GetFileSystem(context);
            auto path = std::filesystem::path(bind.data_dir) /
                        bind.objects[state.current_object].file_path;
            state.reader = std::make_unique<TAEObjectReader>(fs, path.string());
            state.reader->ReadMeta();
            state.current_block = 0;
        }

        // Find next block that passes zone map filters
        while (state.current_block < state.reader->BlockCount()) {
            auto blk = static_cast<uint32_t>(state.current_block);
            state.current_block++;

            if (!state.filters.empty() &&
                !BlockPassesFilters(state.filters, *state.reader, blk)) {
                state.blocks_skipped++;
                continue;
            }

            // Read the block
            state.blocks_scanned++;
            auto decoded_cols = state.reader->ReadBlock(blk, state.read_seqnums);
            if (decoded_cols.empty()) continue;

            duckdb::idx_t row_count = decoded_cols[0].row_count;
            output.SetCardinality(row_count);
            for (duckdb::idx_t i = 0; i < decoded_cols.size(); i++) {
                FillColumn(output.data[i], decoded_cols[i], row_count);
            }
            return; // one block per call
        }

        // Exhausted current object → move to next
        state.current_object++;
        state.reader.reset();
    }

    // No more data
    output.SetCardinality(0);
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
    func.projection_pushdown = true;
    func.filter_pushdown = true;
    func.filter_prune = true;

    return func;
}

} // namespace tae
