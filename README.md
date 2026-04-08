# DuckDB TAE Scanner Extension

A [DuckDB](https://duckdb.org/) extension that reads [MatrixOne](https://github.com/matrixorigin/matrixone) TAE (Table Analytics Engine) object files directly, enabling external query engines to scan MO data without ETL.

Built on DuckDB **v1.5.1** using the official extension build framework.

## Features

| Category | Details |
|----------|---------|
| **Storage format** | Direct TAE binary parsing (header, metadata, column data, zone maps), LZ4 decompression, CRC32-wrapped file support (auto-detected) |
| **I/O** | DuckDB FileSystem abstraction — local files and S3 (`s3://`) paths; `posix_fadvise` prefetching; targeted CRC reads (no full-file strip) |
| **Pushdown** | Projection, zone map predicate, per-row filter, filter-prune (filter-only cols excluded from output), ORDER BY (sorts by zone map stats) |
| **Pruning** | Block-level zone map skip, object-level partition prune (entire files skipped at Init time) |
| **Types** | bool, int8–uint64, float32/64, decimal64/128, date, datetime, timestamp, char, varchar, blob, text, json, uuid, enum, bit; CONSTANT vectors |
| **Parallelism** | Multi-threaded scan with atomic work-unit dispatch |
| **Statistics** | Row-count estimates, column min/max from zone maps, `count(*)` metadata fast path (zero file I/O) |
| **Extras** | Virtual columns (`file_path`, `block_id`), sampling pushdown (`TABLESAMPLE SYSTEM(N%)`), EXPLAIN integration |

## Quick Start

```sql
LOAD 'tae_scanner';

-- Scan a table via manifest
SELECT * FROM tae_scan('/path/to/manifest.json');

-- Predicates push down to zone maps + per-row filter
SELECT * FROM tae_scan('/path/to/manifest.json')
WHERE col_date >= DATE '1994-01-01' AND col_int > 100;

-- Virtual columns for provenance tracking
SELECT file_path, block_id, col_int FROM tae_scan('/path/to/manifest.json');

-- Sampling
SELECT * FROM tae_scan('/path/to/manifest.json') TABLESAMPLE SYSTEM(10%);

-- S3 (requires httpfs extension)
SELECT count(*) FROM tae_scan('s3://bucket/manifests/lineitem.json');
```

The manifest JSON describes the schema, object list, and data directory. Manifests are generated via MO's debug HTTP API (`/debug/tae/manifest`).

## Building

This extension uses DuckDB's standard extension build system. DuckDB v1.5.1 is included as a Git submodule.

### Prerequisites

| Dependency | Version | Notes |
|-----------|---------|-------|
| **C++ compiler** | C++17 | clang++ recommended |
| **CMake** | ≥ 3.14 | |
| **LZ4** | any | `sudo apt install liblz4-dev` (Ubuntu) |
| **Python 3** | ≥ 3.6 | For test data generation only |

### Build

```bash
git clone --recurse-submodules https://github.com/matrixorigin/duckdb-tae-scanner.git
cd duckdb-tae-scanner

# Using the DuckDB extension Makefile
make release
```

The loadable extension is built at:
```
build/release/extension/tae_scanner/tae_scanner.duckdb_extension
```

### Alternative: Direct CMake

```bash
# Initialize submodules if not done
git submodule update --init --recursive

# Build DuckDB + extension together
cd duckdb
GEN=ninja EXTENSION_CONFIGS="../extension_config.cmake" make release
```

### Running Tests

```bash
# Generate test data first
python3 test/gen_test_data.py

# Build and run
make test
```

#### Test tags

```bash
./build/release/test/tae_tests "[scan]"            # Basic scan
./build/release/test/tae_tests "[filter]"          # Filter pushdown
./build/release/test/tae_tests "[filter_prune]"    # Filter-prune optimization
./build/release/test/tae_tests "[types]"           # Type mapping
./build/release/test/tae_tests "[datetime]"        # Date/timestamp
./build/release/test/tae_tests "[lz4]"             # LZ4 compression
./build/release/test/tae_tests "[orderby]"         # ORDER BY pushdown
./build/release/test/tae_tests "[partition_prune]" # Object-level pruning
./build/release/test/tae_tests "[stats]"           # Planner statistics
./build/release/test/tae_tests "[reader]"          # Low-level reader
./build/release/test/tae_tests "[zonemap]"         # Zone map evaluation
./build/release/test/tae_tests "[error]"           # Error handling
```

### Loading in DuckDB

```bash
# Start DuckDB with unsigned extension loading
./duckdb --unsigned

# Load the extension
LOAD '/path/to/tae_scanner.duckdb_extension';

# Query
SELECT count(*) FROM tae_scan('/path/to/manifest.json');
```

## Type Mapping

| MO Type | DuckDB Type | Notes |
|---------|-------------|-------|
| bool, int8–int64, uint8–uint64, float32/64 | Native types | Direct `memcpy` |
| decimal64 | DECIMAL(w,s) | 8-byte `memcpy` |
| decimal128 | DECIMAL(w,s) | 16-byte `memcpy` |
| date | DATE | Epoch offset adjustment |
| datetime, timestamp | TIMESTAMP | Epoch offset adjustment |
| char, varchar, text, json | VARCHAR | Varlena decode |
| blob, binary, varbinary | BLOB | Varlena decode |
| uuid | UUID | Big-endian → `hugeint_t` |
| enum | USMALLINT | `memcpy` |
| bit | UBIGINT | `memcpy` |

All types support filter pushdown, null bitmaps, CONSTANT vectors, and planner statistics.

## Manifest Format

```json
{
  "database": "tpch",
  "table": "lineitem",
  "data_dir": "/path/to/data",
  "sort_column": "l_orderkey",
  "columns": [
    {"name": "l_orderkey", "oid": 23},
    {"name": "l_partkey", "oid": 23},
    {"name": "l_quantity", "oid": 32, "width": 15, "scale": 2}
  ],
  "objects": [
    {"path": "obj_001.tae", "rows": 8192, "blocks": 4, "size": 524288}
  ]
}
```

| Field | Required | Description |
|-------|----------|-------------|
| `columns[].name` | yes | Column name |
| `columns[].oid` | yes | MO type OID (see `tae_types.hpp`) |
| `columns[].width` | decimal | Decimal precision |
| `columns[].scale` | decimal | Decimal scale |
| `objects[].path` | yes | Object file path (relative to `data_dir` or manifest dir) |
| `objects[].rows` | yes | Total row count |
| `objects[].blocks` | yes | Block count |
| `data_dir` | no | Base directory for object paths (default: manifest dir) |
| `sort_column` | no | Column name for ORDER BY pushdown hint |

## Project Structure

```
├── CMakeLists.txt              # Extension build rules (LZ4, C++17)
├── Makefile                    # DuckDB extension build driver
├── extension_config.cmake      # Extension registration
├── duckdb/                     # DuckDB v1.5.1 (submodule)
├── extension-ci-tools/         # DuckDB CI tooling (submodule)
├── include/
│   ├── tae_scanner.hpp         # tae_scan() table function
│   ├── tae_object_reader.hpp   # TAE binary format reader
│   ├── tae_column_fill.hpp     # Column data → DuckDB vector fill
│   ├── tae_filter.hpp          # Per-row filter evaluation
│   ├── tae_types.hpp           # MO type OID → DuckDB type mapping
│   ├── tae_zonemap.hpp         # Zone map predicate evaluation
│   └── tae_scanner_extension.hpp
├── src/
│   ├── tae_scanner.cpp         # Table function (bind, init, scan, statistics)
│   ├── tae_object_reader.cpp   # Binary format parser, CRC handling
│   ├── tae_column_fill.cpp     # Type-specific column filling
│   ├── tae_filter.cpp          # Row-level filter engine
│   └── tae_scanner_extension.cpp  # Extension entry point
└── test/
    ├── gen_test_data.py        # Test data generator
    ├── test_scan_basic.cpp     # Scan, projection, parallel, explain
    ├── test_scan_filters.cpp   # LZ4, ORDER BY, filter-prune, stats, partition prune
    ├── test_scan_types.cpp     # Decimal, UUID, blob, date, timestamp
    ├── test_scan_errors.cpp    # Error handling
    ├── test_reader.cpp         # Low-level reader, coalescing, prefetch
    └── test_zonemap.cpp        # Zone map evaluation
```

## License

Apache License 2.0 — see [LICENSE](LICENSE)
