// Copyright 2024 Matrix Origin
// SPDX-License-Identifier: Apache-2.0
//
// MO TAE type system → DuckDB LogicalType mapping.
// Reference: matrixone/pkg/container/types/types.go

#pragma once

#include "duckdb.hpp"
#include <cstdint>
#include <cstring>

namespace tae {

// ---------------------------------------------------------------------------
// MO type OIDs (pkg/container/types/types.go lines 31-96)
// ---------------------------------------------------------------------------
enum MOTypeOid : uint8_t {
    MO_T_any           = 0,
    MO_T_bool          = 10,
    MO_T_bit           = 11,
    MO_T_int8          = 20,
    MO_T_int16         = 21,
    MO_T_int32         = 22,
    MO_T_int64         = 23,
    MO_T_int128        = 24,
    MO_T_uint8         = 25,
    MO_T_uint16        = 26,
    MO_T_uint32        = 27,
    MO_T_uint64        = 28,
    MO_T_uint128       = 29,
    MO_T_float32       = 30,
    MO_T_float64       = 31,
    MO_T_decimal64     = 32,
    MO_T_decimal128    = 33,
    MO_T_decimal256    = 34,
    MO_T_date          = 50,
    MO_T_time          = 51,
    MO_T_datetime      = 52,
    MO_T_timestamp     = 53,
    MO_T_interval      = 54,
    MO_T_year          = 55,
    MO_T_char          = 60,
    MO_T_varchar       = 61,
    MO_T_json          = 62,
    MO_T_uuid          = 63,
    MO_T_binary        = 64,
    MO_T_varbinary     = 65,
    MO_T_enum          = 66,
    MO_T_blob          = 70,
    MO_T_text          = 71,
    MO_T_datalink      = 72,
    MO_T_TS            = 100,
    MO_T_Rowid         = 101,
    MO_T_Blockid       = 102,
    MO_T_Objectid      = 103,
    MO_T_array_float32 = 224,
    MO_T_array_float64 = 225,
};

// ---------------------------------------------------------------------------
// MO Type struct — 16 bytes, native-endian, memory-mapped directly from disk
// (pkg/container/types/types.go Type struct, encoded via unsafe.Pointer cast)
// ---------------------------------------------------------------------------
struct __attribute__((packed)) MOType {
    uint8_t  oid;       // MOTypeOid
    uint8_t  charset;
    uint8_t  not_null;
    uint8_t  dummy2;
    int32_t  size;      // element size in bytes (-24 for varlena)
    int32_t  width;     // precision for DECIMAL
    int32_t  scale;     // scale for DECIMAL
};
static_assert(sizeof(MOType) == 16, "MOType must be 16 bytes");

// ---------------------------------------------------------------------------
// Epoch offsets: MO counts from Jan 1, year 1 AD (Gregorian proleptic)
// DuckDB counts from Jan 1, 1970 (Unix epoch)
// ---------------------------------------------------------------------------
constexpr int32_t MO_UNIX_EPOCH_DAYS = 719162;  // DateFromCalendar(1970,1,1)
constexpr int64_t MO_UNIX_EPOCH_USEC = 62135596800000000LL;  // DatetimeFromClock(1970,1,1,0,0,0,0)

// ---------------------------------------------------------------------------
// Varlena constants (pkg/container/types/bytes.go)
// ---------------------------------------------------------------------------
constexpr uint32_t VARLENA_SIZE        = 24;
constexpr uint32_t VARLENA_INLINE_MAX  = 23;
constexpr uint32_t VARLENA_BIG_MARKER  = 0xFFFFFFFF;

struct __attribute__((packed)) Varlena {
    uint8_t data[VARLENA_SIZE];

    bool is_inline() const { return data[0] <= VARLENA_INLINE_MAX; }

    // Inline: length = data[0], bytes = &data[1]
    const char *inline_data() const { return reinterpret_cast<const char *>(&data[1]); }
    uint32_t inline_length() const { return data[0]; }

    // Big: marker(4) + offset(4) + length(4), actual data in area buffer
    uint32_t big_offset() const {
        uint32_t v;
        memcpy(&v, &data[4], 4);
        return v;
    }
    uint32_t big_length() const {
        uint32_t v;
        memcpy(&v, &data[8], 4);
        return v;
    }
};
static_assert(sizeof(Varlena) == 24, "Varlena must be 24 bytes");

// ---------------------------------------------------------------------------
// MOTypeOid → DuckDB LogicalType
// ---------------------------------------------------------------------------
inline duckdb::LogicalType MOTypeToDuckDB(const MOType &t) {
    using namespace duckdb;
    switch (static_cast<MOTypeOid>(t.oid)) {
    case MO_T_bool:       return LogicalType::BOOLEAN;
    case MO_T_int8:       return LogicalType::TINYINT;
    case MO_T_int16:      return LogicalType::SMALLINT;
    case MO_T_int32:      return LogicalType::INTEGER;
    case MO_T_int64:      return LogicalType::BIGINT;
    case MO_T_uint8:      return LogicalType::UTINYINT;
    case MO_T_uint16:     return LogicalType::USMALLINT;
    case MO_T_uint32:     return LogicalType::UINTEGER;
    case MO_T_uint64:     return LogicalType::UBIGINT;
    case MO_T_float32:    return LogicalType::FLOAT;
    case MO_T_float64:    return LogicalType::DOUBLE;
    case MO_T_decimal64:  return LogicalType::DECIMAL(t.width, t.scale);
    case MO_T_decimal128: return LogicalType::DECIMAL(t.width, t.scale);
    case MO_T_date:       return LogicalType::DATE;
    case MO_T_datetime:   return LogicalType::TIMESTAMP;
    case MO_T_timestamp:  return LogicalType::TIMESTAMP;
    case MO_T_time:       return LogicalType::TIME;
    case MO_T_year:       return LogicalType::SMALLINT;
    case MO_T_char:
    case MO_T_varchar:
    case MO_T_text:
    case MO_T_json:
    case MO_T_datalink:   return LogicalType::VARCHAR;
    case MO_T_blob:
    case MO_T_binary:
    case MO_T_varbinary:  return LogicalType::BLOB;
    case MO_T_uuid:       return LogicalType::UUID;
    case MO_T_bit:        return LogicalType::UBIGINT;
    case MO_T_enum:       return LogicalType::USMALLINT;
    case MO_T_array_float32: return LogicalType::LIST(LogicalType::FLOAT);
    case MO_T_array_float64: return LogicalType::LIST(LogicalType::DOUBLE);
    default:
        throw std::runtime_error("unsupported MO type OID: " + std::to_string(t.oid));
    }
}

// Fixed-size in bytes for a given MO type (-1 = varlena)
inline int32_t MOTypeFixedSize(MOTypeOid oid) {
    switch (oid) {
    case MO_T_bool: case MO_T_int8: case MO_T_uint8:       return 1;
    case MO_T_int16: case MO_T_uint16: case MO_T_year: case MO_T_enum: return 2;
    case MO_T_int32: case MO_T_uint32: case MO_T_date: case MO_T_float32: return 4;
    case MO_T_int64: case MO_T_uint64: case MO_T_float64:
    case MO_T_datetime: case MO_T_timestamp: case MO_T_time:
    case MO_T_decimal64: case MO_T_bit:                     return 8;
    case MO_T_decimal128: case MO_T_uuid:                   return 16;
    case MO_T_decimal256:                                   return 32;
    default:                                                return -1; // varlena
    }
}

} // namespace tae
