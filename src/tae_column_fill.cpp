// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0
//
// Column fill helpers: copy decoded TAE columns into DuckDB vectors.

#include "tae_column_fill.hpp"
#include "tae_types.hpp"

#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/vector/constant_vector.hpp"

#include <cstring>

namespace tae {

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

// MO stores UUID as [16]byte (RFC 4122 big-endian).
// DuckDB stores UUID as hugeint_t with MSB flipped for sort order.
static void CopyUuidColumn(duckdb::Vector &out_vec,
                            const DecodedColumn &col,
                            duckdb::idx_t count) {
    auto *dst = duckdb::FlatVector::GetData<duckdb::hugeint_t>(out_vec);
    const uint8_t *src = col.data.data();
    for (duckdb::idx_t i = 0; i < count; i++) {
        dst[i] = duckdb::UUID::FromBlob(src + i * 16);
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

// Fill a DuckDB CONSTANT_VECTOR from a single-element DecodedColumn
static void FillConstantColumn(duckdb::Vector &out_vec,
                               const DecodedColumn &col) {
    out_vec.SetVectorType(duckdb::VectorType::CONSTANT_VECTOR);

    // Constant NULL: data is empty, null_count > 0
    if (col.data.empty()) {
        duckdb::ConstantVector::SetNull(out_vec, true);
        return;
    }

    // Check if position 0 is null in bitmap
    bool is_null = false;
    if (col.null_count > 0 && !col.null_bitmap.empty()) {
        is_null = (col.null_bitmap[0] & 1ULL) != 0;
    }
    if (is_null) {
        duckdb::ConstantVector::SetNull(out_vec, true);
        return;
    }

    auto oid = static_cast<MOTypeOid>(col.type.oid);
    switch (oid) {
    case MO_T_date: {
        auto *src = reinterpret_cast<const int32_t *>(col.data.data());
        auto *dst = duckdb::ConstantVector::GetData<int32_t>(out_vec);
        dst[0] = src[0] - MO_UNIX_EPOCH_DAYS;
        break;
    }
    case MO_T_datetime:
    case MO_T_timestamp: {
        auto *src = reinterpret_cast<const int64_t *>(col.data.data());
        auto *dst = duckdb::ConstantVector::GetData<int64_t>(out_vec);
        dst[0] = src[0] - MO_UNIX_EPOCH_USEC;
        break;
    }
    case MO_T_char:
    case MO_T_varchar:
    case MO_T_blob:
    case MO_T_text:
    case MO_T_json:
    case MO_T_binary:
    case MO_T_varbinary:
    case MO_T_datalink: {
        auto *src = reinterpret_cast<const Varlena *>(col.data.data());
        const uint8_t *area = col.area.data();
        const auto &v = src[0];
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
        duckdb::ConstantVector::GetData<duckdb::string_t>(out_vec)[0] = target;
        break;
    }
    case MO_T_uuid: {
        auto *dst = duckdb::ConstantVector::GetData<duckdb::hugeint_t>(out_vec);
        dst[0] = duckdb::UUID::FromBlob(col.data.data());
        break;
    }
    default: {
        auto elem_size = MOTypeFixedSize(oid);
        if (elem_size <= 0) break;
        auto *dst = duckdb::ConstantVector::GetData(out_vec);
        memcpy(dst, col.data.data(), static_cast<size_t>(elem_size));
        break;
    }
    }
}

void FillColumn(duckdb::Vector &out_vec,
                const DecodedColumn &col,
                duckdb::idx_t count) {
    // CONSTANT vector: single value for all rows
    if (col.vec_class == 1) {
        FillConstantColumn(out_vec, col);
        return;
    }

    // FLAT vector: per-row data
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
    case MO_T_uuid:
        CopyUuidColumn(out_vec, col, count);
        break;
    default:
        CopyFixedColumn(out_vec, col, count);
        break;
    }
    SetNullMask(out_vec, col, count);
}

} // namespace tae
