// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0
//
// Column fill helpers: copy decoded TAE columns into DuckDB vectors.

#pragma once

#include "tae_object_reader.hpp"
#include "duckdb.hpp"

namespace tae {

// Fill a DuckDB vector from a decoded TAE column.
// Handles FLAT and CONSTANT vectors, all MO types, and null bitmaps.
// `src_offset` is the row offset into the decoded column (for chunking
// 8192-row TAE blocks into STANDARD_VECTOR_SIZE chunks).
void FillColumn(duckdb::Vector &out_vec, const DecodedColumn &col,
                duckdb::idx_t count, duckdb::idx_t src_offset = 0);

} // namespace tae
