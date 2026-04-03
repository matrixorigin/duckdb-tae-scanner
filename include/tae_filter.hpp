// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0
//
// Filter encoding, extraction, zone map evaluation, and per-row filtering.

#pragma once

#include "tae_scanner.hpp"
#include "tae_object_reader.hpp"
#include "tae_zonemap.hpp"
#include "duckdb.hpp"

namespace tae {

// Is this MO type OID a string/varlena type?
bool IsStringType(uint8_t oid);

// Extract DuckDB TableFilter(s) into PushedFilter list.
void ExtractFilter(const duckdb::TableFilter &filter,
                   uint16_t col_idx, uint16_t seqnum, uint8_t mo_oid,
                   std::vector<PushedFilter> &out);

// Check all pushed filters against a block's zone maps; return false to skip.
bool BlockPassesFilters(const std::vector<PushedFilter> &filters,
                        const TAEObjectReader &reader,
                        uint32_t block_idx);

// Apply all pushed filters per-row, compacting the output chunk.
// Returns new row count after filtering.
duckdb::idx_t ApplyRowFilters(const std::vector<PushedFilter> &filters,
                               const std::vector<DecodedColumn> &decoded_cols,
                               duckdb::DataChunk &output,
                               duckdb::idx_t row_count);

// Decode a fixed-width zone-map value to a DuckDB Value.
duckdb::Value ZoneMapBytesToValue(const uint8_t *ptr, MOTypeOid oid,
                                   const duckdb::LogicalType &col_type);

// Decode a string zone-map value (first 32 bytes = varlena-style prefix).
std::string ZoneMapBytesToString(const uint8_t *ptr);

} // namespace tae
