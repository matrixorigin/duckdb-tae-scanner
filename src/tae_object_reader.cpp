// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0
//
// TAE object file reader implementation.
// Uses DuckDB FileSystem for transparent local/S3/HTTP support.

#include "tae_object_reader.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

// LZ4 decompression
#include <lz4.h>

namespace tae {

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

TAEObjectReader::TAEObjectReader(duckdb::FileSystem &fs, const std::string &file_path)
    : fs_(fs), file_path_(file_path), file_size_(0), meta_{} {
    file_handle_ = fs_.OpenFile(file_path, duckdb::FileOpenFlags::FILE_FLAGS_READ);
    file_size_ = fs_.GetFileSize(*file_handle_);
}

TAEObjectReader::~TAEObjectReader() {
    // FileHandle closes automatically via destructor
}

// ---------------------------------------------------------------------------
// ReadBytes — read raw bytes from file at a given offset
// Uses DuckDB FileSystem random-access read (works for local, S3, HTTP, etc.)
// ---------------------------------------------------------------------------

std::vector<uint8_t> TAEObjectReader::ReadBytes(uint64_t offset, uint64_t length) {
    std::vector<uint8_t> buf(length);
    fs_.Read(*file_handle_, buf.data(), static_cast<int64_t>(length), offset);
    return buf;
}

// ---------------------------------------------------------------------------
// LZ4 Decompression
// ---------------------------------------------------------------------------

std::vector<uint8_t> TAEObjectReader::DecompressLZ4(const uint8_t *src,
                                                     uint32_t src_len,
                                                     uint32_t origin_size) {
    std::vector<uint8_t> dst(origin_size);
    int decoded = LZ4_decompress_safe(
        reinterpret_cast<const char *>(src),
        reinterpret_cast<char *>(dst.data()),
        static_cast<int>(src_len),
        static_cast<int>(origin_size));
    if (decoded < 0 || static_cast<uint32_t>(decoded) != origin_size) {
        throw std::runtime_error("LZ4 decompression failed: expected " +
                                 std::to_string(origin_size) + " got " +
                                 std::to_string(decoded));
    }
    return dst;
}

// ---------------------------------------------------------------------------
// ParseMetadata — parse objectMetaV3 binary format
//
// After IOEntryHeader stripping, the buffer is an objectMetaV3:
//   [v3 header 32B] → points to DataMeta offset
//   DataMeta (objectDataMetaV1):
//     [BlockHeader 179B] [ColumnMeta × metaColCount (124B each)]
//     ... repeated for object-level
//     [BlockIndex: blockCount(4B) + {offset(4B), length(4B)} × blockCount]
//     Per-block data: each block has [BlockHeader 179B][ColumnMeta × ...]
// ---------------------------------------------------------------------------

void TAEObjectReader::ParseMetadata(const uint8_t *buf, uint32_t len) {
    if (len < META_V3_HEADER_LEN) {
        throw std::runtime_error("metadata too small for v3 header");
    }

    // Read v3 header
    uint16_t data_meta_count;
    uint32_t data_meta_offset;
    memcpy(&data_meta_count, buf + MV3_DATA_COUNT_OFF, 2);
    memcpy(&data_meta_offset, buf + MV3_DATA_OFFSET_OFF, 4);

    if (data_meta_count == 0) {
        meta_.block_count = 0;
        return;
    }

    if (data_meta_offset >= len) {
        throw std::runtime_error("data meta offset out of range");
    }

    const uint8_t *dm = buf + data_meta_offset;
    uint32_t dm_len = len - data_meta_offset;

    // DataMeta = objectDataMetaV1: starts with object-level BlockHeader (179B)
    if (dm_len < BLOCK_HEADER_SIZE) {
        throw std::runtime_error("data meta too small for block header");
    }

    // Object-level BlockHeader: extract metaColumnCount for the object
    uint16_t obj_meta_col_count;
    memcpy(&obj_meta_col_count, dm + BH_META_COL_CNT_OFF, 2);

    // Object-level data length = headerLen + metaColCount * colMetaLen
    uint32_t obj_data_len = BLOCK_HEADER_SIZE + uint32_t(obj_meta_col_count) * COL_META_LEN;
    if (obj_data_len > dm_len) {
        throw std::runtime_error("object data meta exceeds buffer");
    }

    // BlockIndex follows the object-level data
    const uint8_t *bi = dm + obj_data_len;
    uint32_t bi_remaining = dm_len - obj_data_len;
    if (bi_remaining < BI_BLOCK_COUNT_LEN) {
        throw std::runtime_error("no room for block index");
    }

    uint32_t block_count;
    memcpy(&block_count, bi, BI_BLOCK_COUNT_LEN);

    uint32_t bi_total = BI_BLOCK_COUNT_LEN + block_count * BI_POS_LEN;
    if (bi_total > bi_remaining) {
        throw std::runtime_error("block index exceeds buffer");
    }

    meta_.block_count = block_count;
    meta_.blocks.resize(block_count);

    // Parse each block using the block index
    for (uint32_t b = 0; b < block_count; b++) {
        uint32_t pos_off = BI_BLOCK_COUNT_LEN + b * BI_POS_LEN;
        uint32_t blk_offset, blk_length;
        memcpy(&blk_offset, bi + pos_off, 4);
        memcpy(&blk_length, bi + pos_off + 4, 4);

        // blk_offset is relative to the start of DataMeta (dm)
        if (blk_offset + blk_length > dm_len) {
            throw std::runtime_error("block " + std::to_string(b) + " out of range");
        }

        const uint8_t *blk = dm + blk_offset;
        if (blk_length < BLOCK_HEADER_SIZE) {
            throw std::runtime_error("block " + std::to_string(b) + " header too small");
        }

        auto &info = meta_.blocks[b];
        memcpy(&info.rows, blk + BH_ROWS_OFF, 4);
        memcpy(&info.col_count, blk + BH_COL_COUNT_OFF, 2);
        memcpy(&info.meta_col_count, blk + BH_META_COL_CNT_OFF, 2);
        memcpy(&info.max_seqnum, blk + BH_MAX_SEQ_OFF, 2);

        // Parse column metadata (indexed by seqnum, not ordinal position)
        uint32_t num_cols = info.meta_col_count;
        uint32_t expected = BLOCK_HEADER_SIZE + num_cols * COL_META_LEN;
        if (expected > blk_length) {
            // Fall back to what fits
            num_cols = (blk_length - BLOCK_HEADER_SIZE) / COL_META_LEN;
        }

        info.columns.resize(num_cols);
        for (uint32_t c = 0; c < num_cols; c++) {
            const uint8_t *cm = blk + BLOCK_HEADER_SIZE + c * COL_META_LEN;
            auto &col = info.columns[c];
            col.data_type = cm[CM_DATA_TYPE_OFF];
            memcpy(&col.idx, cm + CM_IDX_OFF, 2);
            memcpy(&col.ndv, cm + CM_NDV_OFF, 4);
            memcpy(&col.null_cnt, cm + CM_NULL_CNT_OFF, 4);
            memcpy(&col.location, cm + CM_LOCATION_OFF, sizeof(Extent));
            memcpy(&col.checksum, cm + CM_CHECKSUM_OFF, 4);
            memcpy(col.zone_map, cm + CM_ZONEMAP_OFF, ZM_SIZE);
        }
    }
}

// ---------------------------------------------------------------------------
// ReadMeta — read header (first 64B) then metadata extent
// ---------------------------------------------------------------------------

void TAEObjectReader::ReadMeta() {
    // Step 1: read 64-byte header
    auto hdr_buf = ReadBytes(0, HEADER_SIZE);

    // Check magic (8 bytes, uint64 LE)
    uint64_t magic;
    memcpy(&magic, hdr_buf.data(), 8);
    if (magic != OBJECT_MAGIC) {
        throw std::runtime_error("invalid TAE magic: " +
                                 std::to_string(magic));
    }

    // Read meta extent at offset 10
    Extent meta_extent;
    memcpy(&meta_extent, hdr_buf.data() + HEADER_META_EXTENT_OFF, sizeof(Extent));

    // Step 2: read metadata section
    auto meta_raw = ReadBytes(meta_extent.offset, meta_extent.length);

    // Decompress if needed
    if (meta_extent.is_compressed()) {
        meta_buf_ = DecompressLZ4(meta_raw.data(), meta_extent.length,
                                   meta_extent.origin_size);
    } else {
        meta_buf_ = std::move(meta_raw);
    }

    // Step 3: strip IOEntryHeader and parse
    if (meta_buf_.size() < IO_ENTRY_HEADER_LEN) {
        throw std::runtime_error("metadata too small");
    }
    auto *ioh = reinterpret_cast<const IOEntryHeader *>(meta_buf_.data());
    if (ioh->type != IOET_ObjMeta) {
        throw std::runtime_error("expected IOET_ObjMeta, got " +
                                 std::to_string(ioh->type));
    }

    ParseMetadata(meta_buf_.data() + IO_ENTRY_HEADER_LEN,
                  static_cast<uint32_t>(meta_buf_.size()) - IO_ENTRY_HEADER_LEN);
}

// ---------------------------------------------------------------------------
// GetZoneMap — return 64-byte zone map for predicate pushdown
// ---------------------------------------------------------------------------

const uint8_t *TAEObjectReader::GetZoneMap(uint32_t block_idx,
                                            uint16_t seqnum) const {
    if (block_idx >= meta_.block_count) return nullptr;
    const auto &blk = meta_.blocks[block_idx];
    if (seqnum >= blk.columns.size()) return nullptr;
    return blk.columns[seqnum].zone_map;
}

// ---------------------------------------------------------------------------
// DecodeVector — parse MO vector binary format into DecodedColumn
//
// Vector binary layout (pkg/container/vector/vector.go MarshalBinary):
//   [class 1B] [Type 16B] [length 4B]
//   [dataLen 4B] [data ...]
//   [areaLen 4B] [area ...]
//   [nspLen  4B] [nsp ...]
//   [sorted  1B]
// ---------------------------------------------------------------------------

DecodedColumn TAEObjectReader::DecodeVector(const uint8_t *buf, uint32_t len) {
    DecodedColumn col{};
    const uint8_t *p = buf;
    const uint8_t *end = buf + len;

    // class (1 byte): 0=FLAT, 1=CONSTANT
    if (p + 1 > end) throw std::runtime_error("vector too short for class");
    uint8_t vec_class = *p++;

    // Type (16 bytes) — native endian, direct memory cast
    if (p + sizeof(MOType) > end) throw std::runtime_error("vector too short for type");
    memcpy(&col.type, p, sizeof(MOType));
    p += sizeof(MOType);

    // length (4 bytes) — row count
    if (p + 4 > end) throw std::runtime_error("vector too short for length");
    memcpy(&col.row_count, p, 4);
    p += 4;

    // dataLen (4 bytes) + data
    if (p + 4 > end) throw std::runtime_error("vector too short for dataLen");
    uint32_t data_len;
    memcpy(&data_len, p, 4);
    p += 4;
    if (p + data_len > end) throw std::runtime_error("vector data overflow");
    col.data.assign(p, p + data_len);
    p += data_len;

    // areaLen (4 bytes) + area (varlena overflow)
    if (p + 4 > end) throw std::runtime_error("vector too short for areaLen");
    uint32_t area_len;
    memcpy(&area_len, p, 4);
    p += 4;
    if (p + area_len > end) throw std::runtime_error("vector area overflow");
    col.area.assign(p, p + area_len);
    p += area_len;

    // nspLen (4 bytes) + nulls bitmap
    if (p + 4 > end) throw std::runtime_error("vector too short for nspLen");
    uint32_t nsp_len;
    memcpy(&nsp_len, p, 4);
    p += 4;
    if (p + nsp_len > end) throw std::runtime_error("vector nsp overflow");

    // Parse nulls bitmap V2: [count int64][len uint64][dataSize uint64][uint64 words...]
    col.null_count = 0;
    if (nsp_len > 0 && nsp_len >= 24) {
        int64_t count;
        uint64_t bitmap_len, data_size;
        memcpy(&count, p, 8);
        memcpy(&bitmap_len, p + 8, 8);
        memcpy(&data_size, p + 16, 8);
        col.null_count = static_cast<uint64_t>(count);

        uint32_t words = static_cast<uint32_t>(data_size / 8);
        col.null_bitmap.resize(words);
        if (words > 0) {
            memcpy(col.null_bitmap.data(), p + 24, words * 8);
        }
    }
    p += nsp_len;

    // sorted (1 byte)
    if (p + 1 <= end) {
        col.is_sorted = (*p != 0);
    }

    (void)vec_class; // TODO: handle CONSTANT vectors (single value for all rows)
    return col;
}

// ---------------------------------------------------------------------------
// ReadBlock — read requested columns from a specific block
//
// Uses read coalescing: sorts column extents by file offset, merges nearby
// reads (gap ≤ coalesce_gap_), issues fewer large reads, then slices out
// individual column data. This dramatically reduces S3 round-trips.
// ---------------------------------------------------------------------------

std::vector<DecodedColumn> TAEObjectReader::ReadBlock(
    uint32_t block_idx, const std::vector<uint16_t> &seqnums) {

    if (block_idx >= meta_.block_count) {
        throw std::runtime_error("block index out of range");
    }
    const auto &blk = meta_.blocks[block_idx];

    // Build a list of read requests sorted by file offset
    struct ReadReq {
        uint16_t seq;          // original seqnum
        uint32_t result_idx;   // position in output vector
        uint64_t offset;
        uint32_t length;
    };
    std::vector<ReadReq> reqs;
    reqs.reserve(seqnums.size());

    for (uint32_t i = 0; i < seqnums.size(); i++) {
        uint16_t seq = seqnums[i];
        if (seq >= blk.columns.size()) {
            throw std::runtime_error("column seqnum " + std::to_string(seq) +
                                     " out of range (block has " +
                                     std::to_string(blk.columns.size()) + " columns)");
        }
        const Extent &ext = blk.columns[seq].location;
        reqs.push_back({seq, i, ext.offset, ext.length});
    }

    // Sort by file offset for coalescing
    std::sort(reqs.begin(), reqs.end(),
              [](const ReadReq &a, const ReadReq &b) { return a.offset < b.offset; });

    // Coalesce adjacent/overlapping reads within gap threshold
    struct CoalescedRead {
        uint64_t offset;
        uint32_t length;
        std::vector<uint8_t> data; // filled after read
    };
    std::vector<CoalescedRead> coalesced;
    // Map each req index (in sorted order) to its coalesced read index
    std::vector<uint32_t> req_to_coalesced(reqs.size());

    for (uint32_t i = 0; i < reqs.size(); i++) {
        uint64_t req_end = reqs[i].offset + reqs[i].length;
        if (!coalesced.empty()) {
            auto &last = coalesced.back();
            uint64_t last_end = last.offset + last.length;
            uint64_t gap = (reqs[i].offset > last_end) ? (reqs[i].offset - last_end) : 0;
            if (gap <= coalesce_gap_) {
                // Extend the coalesced read to cover this request
                if (req_end > last.offset + last.length) {
                    last.length = static_cast<uint32_t>(req_end - last.offset);
                }
                req_to_coalesced[i] = static_cast<uint32_t>(coalesced.size() - 1);
                continue;
            }
        }
        req_to_coalesced[i] = static_cast<uint32_t>(coalesced.size());
        coalesced.push_back({reqs[i].offset, reqs[i].length, {}});
    }

    // Issue coalesced reads
    for (auto &cr : coalesced) {
        cr.data = ReadBytes(cr.offset, cr.length);
    }

    // Slice out individual columns and decode
    std::vector<DecodedColumn> result(seqnums.size());

    for (uint32_t i = 0; i < reqs.size(); i++) {
        const auto &req = reqs[i];
        const auto &cr = coalesced[req_to_coalesced[i]];
        const Extent &ext = blk.columns[req.seq].location;

        // Extract this column's bytes from the coalesced buffer
        uint64_t local_off = req.offset - cr.offset;
        const uint8_t *raw = cr.data.data() + local_off;
        uint32_t raw_len = ext.length;

        // Decompress if needed
        std::vector<uint8_t> decompressed;
        const uint8_t *col_data;
        uint32_t col_len;

        if (ext.is_compressed()) {
            decompressed = DecompressLZ4(raw, raw_len, ext.origin_size);
            col_data = decompressed.data();
            col_len = ext.origin_size;
        } else {
            col_data = raw;
            col_len = raw_len;
        }

        // Strip IOEntryHeader (4 bytes)
        if (col_len < IO_ENTRY_HEADER_LEN) {
            throw std::runtime_error("column data too small for IOEntryHeader");
        }
        auto *ioh = reinterpret_cast<const IOEntryHeader *>(col_data);
        if (ioh->type != IOET_ColData) {
            throw std::runtime_error("expected IOET_ColData, got " +
                                     std::to_string(ioh->type));
        }

        // Decode vector binary format
        result[req.result_idx] = DecodeVector(col_data + IO_ENTRY_HEADER_LEN,
                                               col_len - IO_ENTRY_HEADER_LEN);
    }

    return result;
}

} // namespace tae
