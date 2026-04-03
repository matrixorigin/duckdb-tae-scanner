// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0
//
// Tests for zone map evaluation logic.

#include "catch.hpp"
#include "tae_zonemap.hpp"

using namespace tae;

// Helper: build a 64-byte zone map with given min/max for a fixed-width type
template <typename T>
static std::vector<uint8_t> MakeZoneMap(T min_val, T max_val, uint8_t type_oid) {
    std::vector<uint8_t> zm(ZM_SIZE, 0);
    memcpy(&zm[ZM_MIN_OFF], &min_val, sizeof(T));
    zm[ZM_MIN_LEN_OFF] = sizeof(T) & ZM_LEN_MASK;
    memcpy(&zm[ZM_MAX_OFF], &max_val, sizeof(T));
    zm[ZM_MAX_INFO_OFF] = sizeof(T) & ZM_LEN_MASK;
    zm[ZM_SCALE_OFF] = ZM_INIT_MASK; // initialized
    zm[ZM_TYPE_OFF] = type_oid;
    return zm;
}

// Helper: build a string zone map
static std::vector<uint8_t> MakeStringZoneMap(const char *min_str, uint32_t min_len,
                                               const char *max_str, uint32_t max_len,
                                               bool max_truncated = false) {
    std::vector<uint8_t> zm(ZM_SIZE, 0);
    memcpy(&zm[ZM_MIN_OFF], min_str, std::min(min_len, ZM_MIN_LEN));
    zm[ZM_MIN_LEN_OFF] = min_len & ZM_LEN_MASK;
    memcpy(&zm[ZM_MAX_OFF], max_str, std::min(max_len, ZM_MAX_LEN));
    uint8_t max_info = max_len & ZM_LEN_MASK;
    if (max_truncated) max_info |= ZM_TRUNCATED_MASK;
    zm[ZM_MAX_INFO_OFF] = max_info;
    zm[ZM_SCALE_OFF] = ZM_INIT_MASK;
    zm[ZM_TYPE_OFF] = MO_T_varchar;
    return zm;
}

// ===================================================================
// Fixed-width zone map checks
// ===================================================================

TEST_CASE("ZoneMapCheckFixed: int32 range [10, 50]", "[zonemap]") {
    auto data = MakeZoneMap<int32_t>(10, 50, MO_T_int32);
    ZoneMap zm(data.data());

    SECTION("EQUAL within range → keep") {
        REQUIRE(ZoneMapCheckFixed<int32_t>(zm, FilterOp::EQUAL, 30));
    }
    SECTION("EQUAL below range → skip") {
        REQUIRE_FALSE(ZoneMapCheckFixed<int32_t>(zm, FilterOp::EQUAL, 5));
    }
    SECTION("EQUAL above range → skip") {
        REQUIRE_FALSE(ZoneMapCheckFixed<int32_t>(zm, FilterOp::EQUAL, 60));
    }
    SECTION("EQUAL at min boundary → keep") {
        REQUIRE(ZoneMapCheckFixed<int32_t>(zm, FilterOp::EQUAL, 10));
    }
    SECTION("EQUAL at max boundary → keep") {
        REQUIRE(ZoneMapCheckFixed<int32_t>(zm, FilterOp::EQUAL, 50));
    }

    SECTION("GREATER_THAN above max → skip") {
        REQUIRE_FALSE(ZoneMapCheckFixed<int32_t>(zm, FilterOp::GREATER_THAN, 50));
    }
    SECTION("GREATER_THAN at max-1 → keep") {
        REQUIRE(ZoneMapCheckFixed<int32_t>(zm, FilterOp::GREATER_THAN, 49));
    }
    SECTION("GREATER_THAN below min → keep") {
        REQUIRE(ZoneMapCheckFixed<int32_t>(zm, FilterOp::GREATER_THAN, 5));
    }

    SECTION("GREATER_THAN_OR_EQUAL at max → keep") {
        REQUIRE(ZoneMapCheckFixed<int32_t>(zm, FilterOp::GREATER_THAN_OR_EQUAL, 50));
    }
    SECTION("GREATER_THAN_OR_EQUAL above max → skip") {
        REQUIRE_FALSE(ZoneMapCheckFixed<int32_t>(zm, FilterOp::GREATER_THAN_OR_EQUAL, 51));
    }

    SECTION("LESS_THAN below min → skip") {
        REQUIRE_FALSE(ZoneMapCheckFixed<int32_t>(zm, FilterOp::LESS_THAN, 10));
    }
    SECTION("LESS_THAN at min+1 → keep") {
        REQUIRE(ZoneMapCheckFixed<int32_t>(zm, FilterOp::LESS_THAN, 11));
    }

    SECTION("LESS_THAN_OR_EQUAL at min → keep") {
        REQUIRE(ZoneMapCheckFixed<int32_t>(zm, FilterOp::LESS_THAN_OR_EQUAL, 10));
    }
    SECTION("LESS_THAN_OR_EQUAL below min → skip") {
        REQUIRE_FALSE(ZoneMapCheckFixed<int32_t>(zm, FilterOp::LESS_THAN_OR_EQUAL, 9));
    }
}

TEST_CASE("ZoneMapCheckFixed: NOT_EQUAL", "[zonemap]") {
    // Constant range: min == max == 42
    auto data = MakeZoneMap<int32_t>(42, 42, MO_T_int32);
    ZoneMap zm(data.data());

    SECTION("All values equal to constant → skip") {
        REQUIRE_FALSE(ZoneMapCheckFixed<int32_t>(zm, FilterOp::NOT_EQUAL, 42));
    }
    SECTION("All values equal but constant differs → keep") {
        REQUIRE(ZoneMapCheckFixed<int32_t>(zm, FilterOp::NOT_EQUAL, 99));
    }
}

TEST_CASE("ZoneMapCheckFixed: uninitialized zone map → keep", "[zonemap]") {
    std::vector<uint8_t> data(ZM_SIZE, 0); // all zeros, not initialized
    ZoneMap zm(data.data());
    REQUIRE(ZoneMapCheckFixed<int32_t>(zm, FilterOp::EQUAL, 42));
}

TEST_CASE("ZoneMapCheckFixed: float64 range", "[zonemap]") {
    auto data = MakeZoneMap<double>(1.5, 9.5, MO_T_float64);
    ZoneMap zm(data.data());

    REQUIRE(ZoneMapCheckFixed<double>(zm, FilterOp::GREATER_THAN, 1.0));
    REQUIRE_FALSE(ZoneMapCheckFixed<double>(zm, FilterOp::GREATER_THAN, 9.5));
    REQUIRE(ZoneMapCheckFixed<double>(zm, FilterOp::LESS_THAN, 5.0));
    REQUIRE_FALSE(ZoneMapCheckFixed<double>(zm, FilterOp::LESS_THAN, 1.5));
}

TEST_CASE("ZoneMapCheckFixed: IS_NULL/IS_NOT_NULL always keep", "[zonemap]") {
    auto data = MakeZoneMap<int32_t>(10, 50, MO_T_int32);
    ZoneMap zm(data.data());

    REQUIRE(ZoneMapCheckFixed<int32_t>(zm, FilterOp::IS_NULL, 0));
    REQUIRE(ZoneMapCheckFixed<int32_t>(zm, FilterOp::IS_NOT_NULL, 0));
}

// ===================================================================
// String zone map checks
// ===================================================================

TEST_CASE("ZoneMapCheckString: range [apple, mango]", "[zonemap]") {
    auto data = MakeStringZoneMap("apple", 5, "mango", 5);
    ZoneMap zm(data.data());

    SECTION("EQUAL within range → keep") {
        REQUIRE(ZoneMapCheckString(zm, FilterOp::EQUAL, "banana", 6));
    }
    SECTION("EQUAL below range → skip") {
        REQUIRE_FALSE(ZoneMapCheckString(zm, FilterOp::EQUAL, "aaa", 3));
    }
    SECTION("EQUAL above range → skip") {
        REQUIRE_FALSE(ZoneMapCheckString(zm, FilterOp::EQUAL, "zzz", 3));
    }

    SECTION("GREATER_THAN above max → skip") {
        REQUIRE_FALSE(ZoneMapCheckString(zm, FilterOp::GREATER_THAN, "mango", 5));
    }
    SECTION("GREATER_THAN at max-ish → keep") {
        REQUIRE(ZoneMapCheckString(zm, FilterOp::GREATER_THAN, "mang", 4));
    }

    SECTION("LESS_THAN below min → skip") {
        REQUIRE_FALSE(ZoneMapCheckString(zm, FilterOp::LESS_THAN, "apple", 5));
    }
    SECTION("LESS_THAN above min → keep") {
        REQUIRE(ZoneMapCheckString(zm, FilterOp::LESS_THAN, "banana", 6));
    }
}

TEST_CASE("ZoneMapCheckString: truncated max → conservative", "[zonemap]") {
    auto data = MakeStringZoneMap("a", 1, "z", 1, /*truncated=*/true);
    ZoneMap zm(data.data());

    // Truncated max means we can't be sure max isn't bigger → keep
    REQUIRE(ZoneMapCheckString(zm, FilterOp::GREATER_THAN, "z", 1));
}
