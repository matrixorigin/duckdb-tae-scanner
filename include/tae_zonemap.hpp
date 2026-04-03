// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0
//
// Zone map evaluation for predicate pushdown.
// Reads MO zone maps (64 bytes per column per block) and evaluates
// DuckDB TableFilter predicates to skip blocks.
//
// Reference: matrixone/pkg/vm/engine/tae/index/zm.go

#pragma once

#include "tae_types.hpp"
#include <cstdint>
#include <cstring>

namespace tae {

// ---------------------------------------------------------------------------
// ZoneMap: 64-byte min/max summary per column per block
//
// Layout (pkg/vm/engine/tae/index/zm.go):
//   [0..29]   min_value      30 bytes
//   [30]      min_len        lower 5 bits = length of min
//   [31..60]  max_value      30 bytes
//   [61]      max_info       lower 5 bits = length of max, bit 7 = truncated
//   [62]      scale_init     lower 6 bits = scale, bit 7 = initialized
//   [63]      data_type      MO type OID (types.T)
// ---------------------------------------------------------------------------

constexpr uint32_t ZM_SIZE = 64;

// Field offsets
constexpr uint32_t ZM_MIN_OFF      = 0;
constexpr uint32_t ZM_MIN_LEN      = 30;
constexpr uint32_t ZM_MIN_LEN_OFF  = 30;
constexpr uint32_t ZM_MAX_OFF      = 31;
constexpr uint32_t ZM_MAX_LEN      = 30;
constexpr uint32_t ZM_MAX_INFO_OFF = 61;
constexpr uint32_t ZM_SCALE_OFF    = 62;
constexpr uint32_t ZM_TYPE_OFF     = 63;

// Bit masks
constexpr uint8_t ZM_LEN_MASK       = 0x1F;  // lower 5 bits
constexpr uint8_t ZM_TRUNCATED_MASK = 0x80;  // bit 7
constexpr uint8_t ZM_SCALE_MASK     = 0x3F;  // lower 6 bits
constexpr uint8_t ZM_INIT_MASK      = 0x80;  // bit 7

struct ZoneMap {
    const uint8_t *data; // points to 64-byte zone map in metadata

    explicit ZoneMap(const uint8_t *zm) : data(zm) {}

    bool IsInited() const { return (data[ZM_SCALE_OFF] & ZM_INIT_MASK) != 0; }
    uint8_t GetType() const { return data[ZM_TYPE_OFF]; }
    uint8_t GetScale() const { return data[ZM_SCALE_OFF] & ZM_SCALE_MASK; }

    uint32_t MinLen() const { return data[ZM_MIN_LEN_OFF] & ZM_LEN_MASK; }
    uint32_t MaxLen() const { return data[ZM_MAX_INFO_OFF] & ZM_LEN_MASK; }
    bool MaxTruncated() const { return (data[ZM_MAX_INFO_OFF] & ZM_TRUNCATED_MASK) != 0; }

    const uint8_t *MinBuf() const { return data + ZM_MIN_OFF; }
    const uint8_t *MaxBuf() const { return data + ZM_MAX_OFF; }

    // --- Typed accessors for fixed-width types ---

    template <typename T>
    T GetMin() const {
        T val;
        memcpy(&val, data + ZM_MIN_OFF, sizeof(T));
        return val;
    }

    template <typename T>
    T GetMax() const {
        T val;
        memcpy(&val, data + ZM_MAX_OFF, sizeof(T));
        return val;
    }

    // --- Comparison functions for predicate pushdown ---

    // Can this block contain values >= key?  (i.e., is max >= key?)
    template <typename T>
    bool AnyGE(T key) const { return GetMax<T>() >= key; }

    // Can this block contain values > key?
    template <typename T>
    bool AnyGT(T key) const { return GetMax<T>() > key; }

    // Can this block contain values <= key?  (i.e., is min <= key?)
    template <typename T>
    bool AnyLE(T key) const { return GetMin<T>() <= key; }

    // Can this block contain values < key?
    template <typename T>
    bool AnyLT(T key) const { return GetMin<T>() < key; }

    // Can this block contain the exact value key? (min <= key <= max)
    template <typename T>
    bool ContainsKey(T key) const { return GetMin<T>() <= key && key <= GetMax<T>(); }

    // String comparison: can block contain values >= key?
    bool AnyGEString(const char *key, uint32_t key_len) const {
        uint32_t max_len = MaxLen();
        int cmp = memcmp(MaxBuf(), key, std::min(max_len, key_len));
        if (cmp != 0) return cmp > 0;
        // If truncated, conservatively assume match possible
        if (MaxTruncated()) return true;
        return max_len >= key_len;
    }

    // String comparison: can block contain values <= key?
    bool AnyLEString(const char *key, uint32_t key_len) const {
        uint32_t min_len = MinLen();
        int cmp = memcmp(MinBuf(), key, std::min(min_len, key_len));
        if (cmp != 0) return cmp < 0;
        return min_len <= key_len;
    }

    // String comparison: can block contain values > key?
    bool AnyGTString(const char *key, uint32_t key_len) const {
        uint32_t max_len = MaxLen();
        int cmp = memcmp(MaxBuf(), key, std::min(max_len, key_len));
        if (cmp != 0) return cmp > 0;
        if (MaxTruncated()) return true;
        return max_len > key_len;
    }

    // String comparison: can block contain values < key?
    bool AnyLTString(const char *key, uint32_t key_len) const {
        uint32_t min_len = MinLen();
        int cmp = memcmp(MinBuf(), key, std::min(min_len, key_len));
        if (cmp != 0) return cmp < 0;
        return min_len < key_len;
    }

    // String comparison: can block contain exact value?
    bool ContainsKeyString(const char *key, uint32_t key_len) const {
        return AnyGEString(key, key_len) && AnyLEString(key, key_len);
    }
};

// ---------------------------------------------------------------------------
// Predicate evaluation: check if a zone map can satisfy a DuckDB filter
//
// Returns true if the block MIGHT contain matching rows (keep it).
// Returns false if the block DEFINITELY has no matching rows (skip it).
// ---------------------------------------------------------------------------

enum class FilterOp : uint8_t {
    EQUAL,
    NOT_EQUAL,
    GREATER_THAN,
    GREATER_THAN_OR_EQUAL,
    LESS_THAN,
    LESS_THAN_OR_EQUAL,
    IS_NULL,
    IS_NOT_NULL,
};

// Evaluate a filter against a zone map for a fixed-width numeric type.
template <typename T>
bool ZoneMapCheckFixed(const ZoneMap &zm, FilterOp op, T constant) {
    if (!zm.IsInited()) return true; // not initialized → keep block

    switch (op) {
    case FilterOp::EQUAL:
        return zm.ContainsKey<T>(constant);
    case FilterOp::NOT_EQUAL:
        // Can only skip if min == max == constant (all values are equal to constant)
        return !(zm.GetMin<T>() == constant && zm.GetMax<T>() == constant);
    case FilterOp::GREATER_THAN:
        return zm.AnyGT<T>(constant);
    case FilterOp::GREATER_THAN_OR_EQUAL:
        return zm.AnyGE<T>(constant);
    case FilterOp::LESS_THAN:
        return zm.AnyLT<T>(constant);
    case FilterOp::LESS_THAN_OR_EQUAL:
        return zm.AnyLE<T>(constant);
    case FilterOp::IS_NULL:
    case FilterOp::IS_NOT_NULL:
        return true; // zone maps don't track nullability per block
    }
    return true; // unknown op → keep block
}

// Evaluate a filter against a zone map for a string type.
inline bool ZoneMapCheckString(const ZoneMap &zm, FilterOp op,
                                const char *constant, uint32_t const_len) {
    if (!zm.IsInited()) return true;

    switch (op) {
    case FilterOp::EQUAL:
        return zm.ContainsKeyString(constant, const_len);
    case FilterOp::NOT_EQUAL: {
        // Skip only if all values are the same string
        uint32_t min_l = zm.MinLen(), max_l = zm.MaxLen();
        if (min_l != max_l || min_l != const_len) return true;
        return memcmp(zm.MinBuf(), constant, const_len) != 0 ||
               memcmp(zm.MaxBuf(), constant, const_len) != 0;
    }
    case FilterOp::GREATER_THAN:
        return zm.AnyGTString(constant, const_len);
    case FilterOp::GREATER_THAN_OR_EQUAL:
        return zm.AnyGEString(constant, const_len);
    case FilterOp::LESS_THAN:
        return zm.AnyLTString(constant, const_len);
    case FilterOp::LESS_THAN_OR_EQUAL:
        return zm.AnyLEString(constant, const_len);
    case FilterOp::IS_NULL:
    case FilterOp::IS_NOT_NULL:
        return true;
    }
    return true;
}

} // namespace tae
