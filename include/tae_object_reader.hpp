// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0
//
// TAE object file reader — reads MatrixOne TAE format via DuckDB FileSystem.
// Supports local files, S3, HTTP, and any DuckDB-registered filesystem.
//
// TAE Object Layout:
//   [Header 64B] [Block0/Col0] [Block0/Col1] ... [BF] [ZoneMaps] [Meta] [Footer 64B]
//
// Reference: matrixone/pkg/objectio/

#pragma once

#include "tae_types.hpp"
#include "tae_zonemap.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#ifdef __linux__
#include <fcntl.h>
#endif

#include "duckdb/common/file_system.hpp"

namespace tae {

// ---------------------------------------------------------------------------
// Extent: 13 bytes — locator for a contiguous region in the object file
// (pkg/objectio/extent.go)
// ---------------------------------------------------------------------------
struct __attribute__((packed)) Extent {
    uint8_t  alg;         // compression: 0=None, 1=LZ4
    uint32_t offset;      // byte offset in file
    uint32_t length;      // compressed length
    uint32_t origin_size; // decompressed length

    bool is_compressed() const { return alg == 1; }
};
static_assert(sizeof(Extent) == 13, "Extent must be 13 bytes");

// ---------------------------------------------------------------------------
// IOEntryHeader: 4 bytes — prefixed to every serialized column data block
// (pkg/objectio/codecs.go)
// ---------------------------------------------------------------------------
struct __attribute__((packed)) IOEntryHeader {
    uint16_t type;     // IOET_ColData=2, IOET_ObjMeta=1
    uint16_t version;  // data format version
};
static_assert(sizeof(IOEntryHeader) == 4, "IOEntryHeader must be 4 bytes");

constexpr uint16_t IOET_ObjMeta = 1;
constexpr uint16_t IOET_ColData = 2;

// ---------------------------------------------------------------------------
// Object Header: first 64 bytes of file
// (pkg/objectio/object.go, meta.go)
//
// Layout: [magic 8B (uint64)] [version 2B (uint16)] [meta_extent 13B] [reserved 41B]
// ---------------------------------------------------------------------------
constexpr uint64_t OBJECT_MAGIC   = 0xFFFFFFFF;
constexpr uint16_t OBJECT_VERSION = 1;
constexpr uint32_t HEADER_SIZE    = 64;

// Meta extent is at offset 10 (after 8B magic + 2B version)
constexpr uint32_t HEADER_META_EXTENT_OFF = 10;

// ---------------------------------------------------------------------------
// Metadata binary layout sizes (from pkg/objectio/)
//
// objectMetaV3 header: 32B
//   [dataMetaCount 2B][dataMetaOffset 4B]
//   [tombstoneMetaCount 2B][tombstoneMetaOffset 4B]
//   [dummy 20B]
//
// BlockHeader: 179B (from pkg/objectio/meta.go)
//   [dbID 8B][tableID 8B][blockID 20B][rows 4B][columnCount 2B]
//   [metaLocation 13B][bloomFilter 13B][bloomChecksum 4B]
//   [zoneMapArea 64B][zoneMapChecksum 4B]
//   [metaColCnt 2B][maxSeq 2B][startID 2B]
//   [appendable 1B][sortKey 2B][bloomFilterType 1B]
//   [dummy 29B]
//
// ColumnMeta: 124B (from pkg/objectio/column.go)
//   [dataType 1B][idx 2B][ndv 4B][nullCnt 4B]
//   [location 13B (Extent)][checksum 4B]
//   [zoneMap 64B][dummy 32B]
// ---------------------------------------------------------------------------
constexpr uint32_t META_V3_HEADER_LEN  = 32;
constexpr uint32_t BLOCK_HEADER_SIZE   = 179;
constexpr uint32_t COL_META_LEN        = 124;
constexpr uint32_t IO_ENTRY_HEADER_LEN = 4;

// BlockHeader field offsets
constexpr uint32_t BH_ROWS_OFF          = 36;   // 8+8+20
constexpr uint32_t BH_COL_COUNT_OFF     = 40;   // 36+4
constexpr uint32_t BH_META_COL_CNT_OFF  = 140;  // metaColumnCount (may differ from colCount)
constexpr uint32_t BH_MAX_SEQ_OFF       = 142;

// ColumnMeta field offsets
constexpr uint32_t CM_DATA_TYPE_OFF = 0;
constexpr uint32_t CM_IDX_OFF       = 1;
constexpr uint32_t CM_NDV_OFF       = 3;
constexpr uint32_t CM_NULL_CNT_OFF  = 7;
constexpr uint32_t CM_LOCATION_OFF  = 11;
constexpr uint32_t CM_CHECKSUM_OFF  = 24;
constexpr uint32_t CM_ZONEMAP_OFF   = 28;

// objectMetaV3 header offsets
constexpr uint32_t MV3_DATA_COUNT_OFF      = 0;
constexpr uint32_t MV3_DATA_OFFSET_OFF     = 2;
constexpr uint32_t MV3_TOMB_COUNT_OFF      = 6;
constexpr uint32_t MV3_TOMB_OFFSET_OFF     = 8;

// BlockIndex offsets
constexpr uint32_t BI_BLOCK_COUNT_LEN = 4;
constexpr uint32_t BI_POS_LEN         = 8; // offset(4B) + length(4B)

// ---------------------------------------------------------------------------
// Parsed column metadata (extracted from raw 124B ColumnMeta)
// ---------------------------------------------------------------------------
struct ColumnMetaInfo {
    uint8_t  data_type;   // MO type OID
    uint16_t idx;         // column seqnum
    uint32_t ndv;
    uint32_t null_cnt;
    Extent   location;    // where column data lives in file
    uint32_t checksum;
    uint8_t  zone_map[ZM_SIZE]; // 64-byte zone map
};

// ---------------------------------------------------------------------------
// Parsed block info
// ---------------------------------------------------------------------------
struct BlockInfo {
    uint32_t rows;
    uint16_t col_count;      // user columns
    uint16_t meta_col_count; // metadata columns (may include internal cols)
    uint16_t max_seqnum;
    std::vector<ColumnMetaInfo> columns; // indexed by seqnum
};

// ---------------------------------------------------------------------------
// Parsed object metadata
// ---------------------------------------------------------------------------
struct ObjectMeta {
    uint32_t               block_count;
    std::vector<BlockInfo> blocks;
};

// ---------------------------------------------------------------------------
// Decoded vector from a single column in a single block
// ---------------------------------------------------------------------------
struct DecodedColumn {
    MOType                    type;
    uint32_t                  row_count;
    std::vector<uint8_t>      data;        // fixed-width: N * elem_size bytes (or 1 elem for CONSTANT)
    std::vector<uint8_t>      area;        // varlena overflow area
    std::vector<uint64_t>     null_bitmap; // bit set = null
    uint64_t                  null_count;
    bool                      is_sorted;
    uint8_t                   vec_class = 0; // 0=FLAT, 1=CONSTANT
};

// ---------------------------------------------------------------------------
// TAEObjectReader — reads a single TAE object file
//
// Uses DuckDB's FileSystem abstraction so that files can be read from:
//   - Local filesystem (file:// or bare path)
//   - S3 (s3://bucket/key) via httpfs extension
//   - HTTP/HTTPS (https://host/path) via httpfs extension
//   - Any other registered DuckDB filesystem
// ---------------------------------------------------------------------------
class TAEObjectReader {
public:
    // Open via DuckDB FileSystem (supports local, S3, HTTP, etc.)
    TAEObjectReader(duckdb::FileSystem &fs, const std::string &file_path);
    ~TAEObjectReader();

    // Phase 1: read header + metadata (2 read calls)
    void ReadMeta();

    // Phase 2: read specific columns from a block
    // seqnums: which columns to read (0-based seqnum indices)
    // Returns one DecodedColumn per requested seqnum.
    std::vector<DecodedColumn> ReadBlock(uint32_t block_idx,
                                         const std::vector<uint16_t> &seqnums);

    // Accessors
    const ObjectMeta &Meta() const { return meta_; }
    uint32_t BlockCount() const { return meta_.block_count; }
    uint32_t BlockRowCount(uint32_t block_idx) const {
        return block_idx < meta_.blocks.size() ? meta_.blocks[block_idx].rows : 0;
    }

    // Zone map access for predicate pushdown
    // Returns pointer to 64-byte zone map, or nullptr if unavailable
    const uint8_t *GetZoneMap(uint32_t block_idx, uint16_t seqnum) const;

    // Read coalescing: set maximum gap (bytes) between extents that will be
    // merged into a single read. Default 256 KB — good for S3 where each
    // request has ~50-100ms latency. Set to 0 to disable coalescing.
    void SetCoalesceGap(uint32_t gap) { coalesce_gap_ = gap; }
    uint32_t GetCoalesceGap() const { return coalesce_gap_; }

    // Prefetch: files ≤ threshold are read entirely on first access.
    // All subsequent ReadBytes calls are served from cache (zero I/O).
    // Default 4 MB. Set to 0 to disable prefetch.
    void SetPrefetchThreshold(uint64_t bytes) { prefetch_threshold_ = bytes; }
    uint64_t GetPrefetchThreshold() const { return prefetch_threshold_; }

    // Advisory prefetch: hint the OS to read a byte range into page cache.
    // Returns immediately (non-blocking). Effective on local Linux filesystems.
    void PrefetchRange(uint64_t offset, uint64_t length);

    // Prefetch all column extents for a specific block.
    // Useful for look-ahead: call on block N+1 while processing block N.
    void PrefetchBlock(uint32_t block_idx, const std::vector<uint16_t> &seqnums);

private:
    std::vector<uint8_t> ReadBytes(uint64_t offset, uint64_t length);
    static std::vector<uint8_t> DecompressLZ4(const uint8_t *src, uint32_t src_len,
                                               uint32_t origin_size);
    static DecodedColumn DecodeVector(const uint8_t *buf, uint32_t len);

    // Parse objectMetaV3 → DataMeta → per-block metadata
    void ParseMetadata(const uint8_t *buf, uint32_t len);

    duckdb::FileSystem                     &fs_;
    std::string                             file_path_;
    duckdb::unique_ptr<duckdb::FileHandle>  file_handle_;
    uint64_t                                file_size_;
    ObjectMeta                              meta_;
    // Raw metadata buffer kept alive for zone map pointers
    std::vector<uint8_t>                    meta_buf_;
    // Read coalescing gap: merge reads within this many bytes (default 256 KB)
    uint32_t                                coalesce_gap_ = 256 * 1024;
    // Prefetch: entire file cached if ≤ this size (default 4 MB, 0 = disabled)
    uint64_t                                prefetch_threshold_ = 4 * 1024 * 1024;
    bool                                    prefetch_attempted_ = false;
    std::vector<uint8_t>                    prefetch_buf_;
    // Advisory fd for posix_fadvise (Linux only, -1 if unavailable)
    int                                     advise_fd_ = -1;
};

} // namespace tae
