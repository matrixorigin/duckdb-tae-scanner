// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0
//
// Filter encoding, extraction, zone map evaluation, and per-row filtering.

#include "tae_filter.hpp"
#include "tae_types.hpp"

#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/null_filter.hpp"
#include "duckdb/planner/filter/in_filter.hpp"
#include "duckdb/planner/filter/optional_filter.hpp"
#include "duckdb/planner/filter/dynamic_filter.hpp"

#include <cstring>

namespace tae {

// ===================================================================
// Filter conversion helpers
// ===================================================================

bool IsStringType(uint8_t oid) {
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
    case MO_T_bool:    { out_bytes[0] = val.GetValue<bool>() ? 1 : 0; break; }
    case MO_T_int8:    { *reinterpret_cast<int8_t  *>(out_bytes.data()) = val.GetValue<int8_t>();   break; }
    case MO_T_int16:   { *reinterpret_cast<int16_t *>(out_bytes.data()) = val.GetValue<int16_t>();  break; }
    case MO_T_int32:   { *reinterpret_cast<int32_t *>(out_bytes.data()) = val.GetValue<int32_t>();  break; }
    case MO_T_int64:   { *reinterpret_cast<int64_t *>(out_bytes.data()) = val.GetValue<int64_t>();  break; }
    case MO_T_uint8:   { out_bytes[0] = val.GetValue<uint8_t>(); break; }
    case MO_T_uint16:  { *reinterpret_cast<uint16_t *>(out_bytes.data()) = val.GetValue<uint16_t>(); break; }
    case MO_T_uint32:  { *reinterpret_cast<uint32_t *>(out_bytes.data()) = val.GetValue<uint32_t>(); break; }
    case MO_T_uint64:  { *reinterpret_cast<uint64_t *>(out_bytes.data()) = val.GetValue<uint64_t>(); break; }
    case MO_T_float32: { *reinterpret_cast<float    *>(out_bytes.data()) = val.GetValue<float>();    break; }
    case MO_T_float64: { *reinterpret_cast<double   *>(out_bytes.data()) = val.GetValue<double>();   break; }
    case MO_T_date: {
        *reinterpret_cast<int32_t *>(out_bytes.data()) =
            val.GetValue<int32_t>() + MO_UNIX_EPOCH_DAYS;
        break;
    }
    case MO_T_datetime:
    case MO_T_timestamp: {
        *reinterpret_cast<int64_t *>(out_bytes.data()) =
            val.GetValue<int64_t>() + MO_UNIX_EPOCH_USEC;
        break;
    }
    case MO_T_time: {
        *reinterpret_cast<int64_t *>(out_bytes.data()) = val.GetValue<int64_t>();
        break;
    }
    case MO_T_decimal64: {
        int64_t v;
        switch (val.type().InternalType()) {
        case duckdb::PhysicalType::INT16: v = val.GetValueUnsafe<int16_t>(); break;
        case duckdb::PhysicalType::INT32: v = val.GetValueUnsafe<int32_t>(); break;
        case duckdb::PhysicalType::INT64: v = val.GetValueUnsafe<int64_t>(); break;
        default: return false;
        }
        *reinterpret_cast<int64_t *>(out_bytes.data()) = v;
        break;
    }
    case MO_T_decimal128: {
        duckdb::hugeint_t v;
        switch (val.type().InternalType()) {
        case duckdb::PhysicalType::INT16: v = duckdb::hugeint_t(val.GetValueUnsafe<int16_t>()); break;
        case duckdb::PhysicalType::INT32: v = duckdb::hugeint_t(val.GetValueUnsafe<int32_t>()); break;
        case duckdb::PhysicalType::INT64: v = duckdb::hugeint_t(val.GetValueUnsafe<int64_t>()); break;
        case duckdb::PhysicalType::INT128: v = val.GetValueUnsafe<duckdb::hugeint_t>(); break;
        default: return false;
        }
        *reinterpret_cast<duckdb::hugeint_t *>(out_bytes.data()) = v;
        break;
    }
    case MO_T_uuid: {
        auto hi = val.GetValueUnsafe<duckdb::hugeint_t>();
        duckdb::UUID::ToBlob(hi, out_bytes.data());
        break;
    }
    default: return false;
    }
    return true;
}

void ExtractFilter(const duckdb::TableFilter &filter,
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
    case duckdb::TableFilterType::IN_FILTER: {
        auto &inf = filter.Cast<duckdb::InFilter>();
        PushedFilter pf;
        pf.col_idx = col_idx;
        pf.seqnum = seqnum;
        pf.mo_type_oid = mo_oid;
        pf.op = FilterOp::IN_SET;
        pf.const_len = 0;
        pf.in_values.reserve(inf.values.size());
        pf.in_value_lens.reserve(inf.values.size());
        bool ok = true;
        for (auto &val : inf.values) {
            std::vector<uint8_t> bytes;
            uint32_t len = 0;
            if (!EncodeConstant(val, mo_oid, bytes, len)) { ok = false; break; }
            pf.in_values.push_back(std::move(bytes));
            pf.in_value_lens.push_back(len);
        }
        if (ok && !pf.in_values.empty()) {
            out.push_back(std::move(pf));
        }
        break;
    }
    case duckdb::TableFilterType::OPTIONAL_FILTER: {
        auto &of = filter.Cast<duckdb::OptionalFilter>();
        if (of.child_filter) {
            ExtractFilter(*of.child_filter, col_idx, seqnum, mo_oid, out);
        }
        break;
    }
    case duckdb::TableFilterType::DYNAMIC_FILTER: {
        auto &df = filter.Cast<duckdb::DynamicFilter>();
        if (df.filter_data) {
            std::lock_guard<std::mutex> lk(df.filter_data->lock);
            if (df.filter_data->initialized && df.filter_data->filter) {
                ExtractFilter(*df.filter_data->filter, col_idx, seqnum, mo_oid, out);
            }
        }
        break;
    }
    default:
        break;
    }
}

// ===================================================================
// Zone map evaluation
// ===================================================================

// Check if a single encoded scalar passes a zone-map test under `op`.
static bool ZoneMapCheckScalar(const ZoneMap &zm, FilterOp op, uint8_t mo_oid,
                                const uint8_t *bytes, uint32_t len) {
    if (IsStringType(mo_oid)) {
        return ZoneMapCheckString(zm, op, reinterpret_cast<const char *>(bytes), len);
    }
    switch (static_cast<MOTypeOid>(mo_oid)) {
    case MO_T_bool:    return ZoneMapCheckFixed(zm, op, *reinterpret_cast<const bool *>(bytes));
    case MO_T_int8:    return ZoneMapCheckFixed(zm, op, *reinterpret_cast<const int8_t *>(bytes));
    case MO_T_int16:   return ZoneMapCheckFixed(zm, op, *reinterpret_cast<const int16_t *>(bytes));
    case MO_T_int32:
    case MO_T_date:    return ZoneMapCheckFixed(zm, op, *reinterpret_cast<const int32_t *>(bytes));
    case MO_T_int64:
    case MO_T_datetime:
    case MO_T_timestamp:
    case MO_T_time:    return ZoneMapCheckFixed(zm, op, *reinterpret_cast<const int64_t *>(bytes));
    case MO_T_uint8:   return ZoneMapCheckFixed(zm, op, *reinterpret_cast<const uint8_t *>(bytes));
    case MO_T_uint16:  return ZoneMapCheckFixed(zm, op, *reinterpret_cast<const uint16_t *>(bytes));
    case MO_T_uint32:  return ZoneMapCheckFixed(zm, op, *reinterpret_cast<const uint32_t *>(bytes));
    case MO_T_uint64:  return ZoneMapCheckFixed(zm, op, *reinterpret_cast<const uint64_t *>(bytes));
    case MO_T_float32: return ZoneMapCheckFixed(zm, op, *reinterpret_cast<const float *>(bytes));
    case MO_T_float64: return ZoneMapCheckFixed(zm, op, *reinterpret_cast<const double *>(bytes));
    case MO_T_decimal64:  return ZoneMapCheckFixed(zm, op, *reinterpret_cast<const int64_t *>(bytes));
    case MO_T_decimal128: return ZoneMapCheckFixed(zm, op, *reinterpret_cast<const duckdb::hugeint_t *>(bytes));
    case MO_T_uuid:
        return ZoneMapCheckString(zm, op, reinterpret_cast<const char *>(bytes), len);
    default:
        return true;
    }
}

static bool EvalFilterOnZoneMap(const PushedFilter &pf,
                                 const uint8_t *zm_data) {
    if (!zm_data) return true;

    ZoneMap zm(zm_data);
    if (!zm.IsInited()) return true;

    if (pf.op == FilterOp::IS_NULL || pf.op == FilterOp::IS_NOT_NULL) {
        return true;
    }

    if (pf.op == FilterOp::IN_SET) {
        for (std::size_t i = 0; i < pf.in_values.size(); ++i) {
            if (ZoneMapCheckScalar(zm, FilterOp::EQUAL, pf.mo_type_oid,
                                    pf.in_values[i].data(), pf.in_value_lens[i])) {
                return true;
            }
        }
        return false;
    }

    return ZoneMapCheckScalar(zm, pf.op, pf.mo_type_oid,
                               pf.constant.data(), pf.const_len);
}

bool BlockPassesFilters(const std::vector<PushedFilter> &filters,
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

bool ZoneMapPassesFilters(const std::vector<PushedFilter> &filters,
                           const uint8_t *zm_data, uint16_t target_seqnum) {
    if (!zm_data) return true;
    for (auto &f : filters) {
        if (f.seqnum != target_seqnum) continue;
        if (!EvalFilterOnZoneMap(f, zm_data)) return false;
    }
    return true;
}

// ===================================================================
// Per-row filter evaluation
// ===================================================================

template <typename T>
static bool CompareFixed(FilterOp op, const uint8_t *row_ptr, const uint8_t *const_ptr) {
    T row_val = *reinterpret_cast<const T *>(row_ptr);
    T const_val = *reinterpret_cast<const T *>(const_ptr);
    switch (op) {
    case FilterOp::EQUAL:                  return row_val == const_val;
    case FilterOp::NOT_EQUAL:              return row_val != const_val;
    case FilterOp::GREATER_THAN:           return row_val > const_val;
    case FilterOp::GREATER_THAN_OR_EQUAL:  return row_val >= const_val;
    case FilterOp::LESS_THAN:              return row_val < const_val;
    case FilterOp::LESS_THAN_OR_EQUAL:     return row_val <= const_val;
    default: return true;
    }
}

static bool RowPassesFilter(const PushedFilter &pf, const DecodedColumn &col,
                             duckdb::idx_t row) {
    bool is_null = false;
    if (!col.null_bitmap.empty() && row < col.row_count) {
        uint64_t word_idx = row / 64;
        if (word_idx < col.null_bitmap.size()) {
            uint64_t word = col.null_bitmap[word_idx];
            is_null = (word & (1ULL << (row % 64))) != 0;
        }
    }

    if (pf.op == FilterOp::IS_NULL) return is_null;
    if (pf.op == FilterOp::IS_NOT_NULL) return !is_null;
    if (is_null) return false;

    if (pf.op == FilterOp::IN_SET) {
        if (IsStringType(pf.mo_type_oid)) {
            auto *slots = reinterpret_cast<const Varlena *>(col.data.data());
            const Varlena &v = slots[row];
            const char *str;
            uint32_t str_len;
            if (v.is_inline()) {
                str = v.inline_data();
                str_len = v.inline_length();
            } else {
                uint32_t off = v.big_offset();
                str_len = v.big_length();
                if (off + str_len > col.area.size()) return true;
                str = reinterpret_cast<const char *>(col.area.data() + off);
            }
            for (std::size_t i = 0; i < pf.in_values.size(); ++i) {
                if (pf.in_value_lens[i] == str_len &&
                    memcmp(str, pf.in_values[i].data(), str_len) == 0) {
                    return true;
                }
            }
            return false;
        }
        auto elem_size = MOTypeFixedSize(static_cast<MOTypeOid>(pf.mo_type_oid));
        if (elem_size <= 0) return true;
        const uint8_t *row_ptr = col.data.data() + row * static_cast<size_t>(elem_size);
        for (std::size_t i = 0; i < pf.in_values.size(); ++i) {
            const uint8_t *cp = pf.in_values[i].data();
            bool hit = false;
            switch (static_cast<MOTypeOid>(pf.mo_type_oid)) {
            case MO_T_int8:
            case MO_T_uint8:
            case MO_T_bool:
                hit = *row_ptr == *cp;
                break;
            case MO_T_int16:
                hit = *reinterpret_cast<const int16_t *>(row_ptr) ==
                      *reinterpret_cast<const int16_t *>(cp);
                break;
            case MO_T_uint16:
                hit = *reinterpret_cast<const uint16_t *>(row_ptr) ==
                      *reinterpret_cast<const uint16_t *>(cp);
                break;
            case MO_T_int32:
            case MO_T_date:
                hit = *reinterpret_cast<const int32_t *>(row_ptr) ==
                      *reinterpret_cast<const int32_t *>(cp);
                break;
            case MO_T_uint32:
                hit = *reinterpret_cast<const uint32_t *>(row_ptr) ==
                      *reinterpret_cast<const uint32_t *>(cp);
                break;
            case MO_T_int64:
            case MO_T_datetime:
            case MO_T_timestamp:
            case MO_T_time:
            case MO_T_decimal64:
                hit = *reinterpret_cast<const int64_t *>(row_ptr) ==
                      *reinterpret_cast<const int64_t *>(cp);
                break;
            case MO_T_uint64:
                hit = *reinterpret_cast<const uint64_t *>(row_ptr) ==
                      *reinterpret_cast<const uint64_t *>(cp);
                break;
            case MO_T_float32:
                hit = *reinterpret_cast<const float *>(row_ptr) ==
                      *reinterpret_cast<const float *>(cp);
                break;
            case MO_T_float64:
                hit = *reinterpret_cast<const double *>(row_ptr) ==
                      *reinterpret_cast<const double *>(cp);
                break;
            case MO_T_decimal128:
            case MO_T_uuid:
                hit = memcmp(row_ptr, cp, 16) == 0;
                break;
            default: break;
            }
            if (hit) return true;
        }
        return false;
    }

    if (IsStringType(pf.mo_type_oid)) {
        auto *slots = reinterpret_cast<const Varlena *>(col.data.data());
        const Varlena &v = slots[row];
        const char *str;
        uint32_t str_len;
        if (v.is_inline()) {
            str = v.inline_data();
            str_len = v.inline_length();
        } else {
            uint32_t off = v.big_offset();
            str_len = v.big_length();
            if (off + str_len <= col.area.size()) {
                str = reinterpret_cast<const char *>(col.area.data() + off);
            } else {
                return true;
            }
        }
        const char *const_str = reinterpret_cast<const char *>(pf.constant.data());
        int cmp = memcmp(str, const_str, std::min(str_len, pf.const_len));
        if (cmp == 0 && str_len != pf.const_len) {
            cmp = (str_len < pf.const_len) ? -1 : 1;
        }
        switch (pf.op) {
        case FilterOp::EQUAL:                  return cmp == 0;
        case FilterOp::NOT_EQUAL:              return cmp != 0;
        case FilterOp::GREATER_THAN:           return cmp > 0;
        case FilterOp::GREATER_THAN_OR_EQUAL:  return cmp >= 0;
        case FilterOp::LESS_THAN:              return cmp < 0;
        case FilterOp::LESS_THAN_OR_EQUAL:     return cmp <= 0;
        default: return true;
        }
    }

    auto elem_size = MOTypeFixedSize(static_cast<MOTypeOid>(pf.mo_type_oid));
    if (elem_size <= 0) return true;
    const uint8_t *row_ptr = col.data.data() + row * static_cast<size_t>(elem_size);
    const uint8_t *const_ptr = pf.constant.data();

    switch (static_cast<MOTypeOid>(pf.mo_type_oid)) {
    case MO_T_int8:      return CompareFixed<int8_t>(pf.op, row_ptr, const_ptr);
    case MO_T_int16:     return CompareFixed<int16_t>(pf.op, row_ptr, const_ptr);
    case MO_T_int32:
    case MO_T_date:      return CompareFixed<int32_t>(pf.op, row_ptr, const_ptr);
    case MO_T_int64:
    case MO_T_datetime:
    case MO_T_timestamp:
    case MO_T_time:      return CompareFixed<int64_t>(pf.op, row_ptr, const_ptr);
    case MO_T_uint8:     return CompareFixed<uint8_t>(pf.op, row_ptr, const_ptr);
    case MO_T_uint16:    return CompareFixed<uint16_t>(pf.op, row_ptr, const_ptr);
    case MO_T_uint32:    return CompareFixed<uint32_t>(pf.op, row_ptr, const_ptr);
    case MO_T_uint64:    return CompareFixed<uint64_t>(pf.op, row_ptr, const_ptr);
    case MO_T_float32:   return CompareFixed<float>(pf.op, row_ptr, const_ptr);
    case MO_T_float64:   return CompareFixed<double>(pf.op, row_ptr, const_ptr);
    case MO_T_bool:      return CompareFixed<uint8_t>(pf.op, row_ptr, const_ptr);
    case MO_T_decimal64: return CompareFixed<int64_t>(pf.op, row_ptr, const_ptr);
    case MO_T_decimal128: return CompareFixed<duckdb::hugeint_t>(pf.op, row_ptr, const_ptr);
    case MO_T_uuid: {
        int cmp = memcmp(row_ptr, const_ptr, 16);
        switch (pf.op) {
        case FilterOp::EQUAL:                  return cmp == 0;
        case FilterOp::NOT_EQUAL:              return cmp != 0;
        case FilterOp::GREATER_THAN:           return cmp > 0;
        case FilterOp::GREATER_THAN_OR_EQUAL:  return cmp >= 0;
        case FilterOp::LESS_THAN:              return cmp < 0;
        case FilterOp::LESS_THAN_OR_EQUAL:     return cmp <= 0;
        default: return true;
        }
    }
    default: return true;
    }
}

duckdb::idx_t ApplyRowFilters(const std::vector<PushedFilter> &filters,
                               const std::vector<DecodedColumn> &decoded_cols,
                               duckdb::DataChunk &output,
                               duckdb::idx_t row_count,
                               duckdb::idx_t src_offset) {
    if (filters.empty()) return row_count;

    duckdb::SelectionVector sel(row_count);
    duckdb::idx_t pass_count = 0;

    for (duckdb::idx_t row = 0; row < row_count; row++) {
        bool pass = true;
        for (auto &pf : filters) {
            if (pf.col_idx >= decoded_cols.size()) continue;
            if (!RowPassesFilter(pf, decoded_cols[pf.col_idx], src_offset + row)) {
                pass = false;
                break;
            }
        }
        if (pass) {
            sel.set_index(pass_count++, row);
        }
    }

    if (pass_count == row_count) return row_count;
    if (pass_count == 0) {
        output.SetCardinality(0);
        return 0;
    }

    output.Slice(sel, pass_count);
    return pass_count;
}

// ===================================================================
// Zone map value decoding (for statistics)
// ===================================================================

duckdb::Value ZoneMapBytesToValue(const uint8_t *ptr, MOTypeOid oid,
                                   const duckdb::LogicalType &col_type) {
    switch (oid) {
    case MO_T_int8:      { int8_t v;   memcpy(&v, ptr, sizeof(v));  return duckdb::Value::TINYINT(v); }
    case MO_T_int16:     { int16_t v;  memcpy(&v, ptr, sizeof(v));  return duckdb::Value::SMALLINT(v); }
    case MO_T_int32:     { int32_t v;  memcpy(&v, ptr, sizeof(v));  return duckdb::Value::INTEGER(v); }
    case MO_T_int64:     { int64_t v;  memcpy(&v, ptr, sizeof(v));  return duckdb::Value::BIGINT(v); }
    case MO_T_uint8:     { uint8_t v;  memcpy(&v, ptr, sizeof(v));  return duckdb::Value::UTINYINT(v); }
    case MO_T_uint16:    { uint16_t v; memcpy(&v, ptr, sizeof(v));  return duckdb::Value::USMALLINT(v); }
    case MO_T_uint32:    { uint32_t v; memcpy(&v, ptr, sizeof(v));  return duckdb::Value::UINTEGER(v); }
    case MO_T_uint64:    { uint64_t v; memcpy(&v, ptr, sizeof(v));  return duckdb::Value::UBIGINT(v); }
    case MO_T_float32:   { float v;    memcpy(&v, ptr, sizeof(v));  return duckdb::Value::FLOAT(v); }
    case MO_T_float64:   { double v;   memcpy(&v, ptr, sizeof(v));  return duckdb::Value::DOUBLE(v); }
    case MO_T_bool:      { return duckdb::Value::BOOLEAN(ptr[0] != 0); }
    case MO_T_date: {
        int32_t v; memcpy(&v, ptr, sizeof(v));
        return duckdb::Value::DATE(duckdb::Date::EpochDaysToDate(v - MO_UNIX_EPOCH_DAYS));
    }
    case MO_T_datetime:
    case MO_T_timestamp: {
        int64_t v; memcpy(&v, ptr, sizeof(v));
        return duckdb::Value::TIMESTAMP(duckdb::Timestamp::FromEpochMicroSeconds(v - MO_UNIX_EPOCH_USEC));
    }
    case MO_T_time: {
        int64_t v; memcpy(&v, ptr, sizeof(v));
        return duckdb::Value::TIME(duckdb::dtime_t(v));
    }
    case MO_T_decimal64: {
        int64_t v; memcpy(&v, ptr, sizeof(v));
        auto w = duckdb::DecimalType::GetWidth(col_type);
        auto s = duckdb::DecimalType::GetScale(col_type);
        return duckdb::Value::DECIMAL(v, w, s);
    }
    case MO_T_decimal128: {
        duckdb::hugeint_t v; memcpy(&v, ptr, sizeof(v));
        auto w = duckdb::DecimalType::GetWidth(col_type);
        auto s = duckdb::DecimalType::GetScale(col_type);
        return duckdb::Value::DECIMAL(v, w, s);
    }
    case MO_T_uuid: {
        auto v = duckdb::UUID::FromBlob(ptr);
        return duckdb::Value::UUID(v);
    }
    default: return duckdb::Value();
    }
}

// Convert zone map min/max bytes to a std::string.
// Zone map layout: bytes [0..29] = raw value, length stored separately.
// For min: length is at byte offset ZM_MIN_LEN_OFF (30) from the zone map start.
// For max: length is at byte offset ZM_MAX_INFO_OFF (61) from the zone map start.
// We receive a pointer to the start of the value (offset 0 or 31 in the zm).
// The caller must provide the length separately.
std::string ZoneMapBytesToString(const uint8_t *ptr, uint32_t len) {
    return std::string(reinterpret_cast<const char *>(ptr), len);
}

} // namespace tae
