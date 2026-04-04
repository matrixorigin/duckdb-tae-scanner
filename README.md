# DuckDB TAE Scanner Extension

A DuckDB extension that reads [MatrixOne](https://github.com/matrixorigin/matrixone) TAE (Table Analytics Engine) object files directly, enabling external query engines to scan MO data without ETL.

## Features

- **Direct TAE binary format reading** — parses MO's columnar storage format (header, metadata, column data, zone maps)
- **DuckDB FileSystem abstraction** — transparent support for local files and S3 (`s3://`) paths
- **Projection pushdown** — reads only the columns needed by the query
- **Zone map predicate pushdown** — evaluates pushed-down predicates against per-block zone maps to skip irrelevant blocks
- **Object-level partition pruning** — skips entire objects when all blocks fail zone map filters at Init time
- **Per-row filter evaluation** — applies pushed-down comparisons row-by-row after zone map block skip
- **Filter-prune optimization** — filter-only columns excluded from output (DuckDB `filter_prune = true`)
- **ORDER BY pushdown** — sorts work units by zone map statistics to produce approximately-ordered output
- **LZ4 decompression** — handles MO's LZ4-compressed column data
- **Full MO type support** — maps all MO types (bool, int8–uint64, float32/64, decimal64/128, date, datetime, timestamp, char, varchar, blob, text, uuid, enum) to DuckDB logical types
- **CONSTANT vector support** — handles MO constant vectors (single value broadcast to all rows)
- **Virtual columns** — exposes `file_path` and `block_id` as virtual columns for provenance tracking
- **Sampling pushdown** — supports `TABLESAMPLE SYSTEM(N%)` via Bernoulli sampling in the scan
- **Parallel block scanning** — multi-threaded execution with atomic work dispatch
- **Planner statistics** — provides row-count estimates and column-level min/max from zone maps
- **posix_fadvise prefetching** — `FADV_SEQUENTIAL` at open + `FADV_WILLNEED` look-ahead for next block
- **EXPLAIN integration** — shows table name, object count, row count, blocks scanned/skipped in query plans

## Quick Start

```sql
-- Load the extension
LOAD 'tae_scanner.so';

-- Scan a table via manifest file
SELECT * FROM tae_scan('/path/to/manifest.json');

-- With predicates (zone map + row-level pushdown)
SELECT * FROM tae_scan('/path/to/manifest.json')
WHERE col_date >= DATE '1994-01-01' AND col_int > 100;

-- Virtual columns (file provenance)
SELECT file_path, block_id, col_int FROM tae_scan('/path/to/manifest.json');

-- Sampling
SELECT * FROM tae_scan('/path/to/manifest.json') TABLESAMPLE SYSTEM(10%);
```

The manifest JSON file describes the schema, object list, and data directory. Generate it using `tae_manifest_gen.py` against a running MatrixOne instance.

## Building

### Prerequisites

| Dependency | Version | Notes |
|-----------|---------|-------|
| **C++ compiler** | C++17 | clang++ recommended, g++ also works |
| **CMake** | ≥ 3.14 | |
| **DuckDB** | source tree | Must be built first (`make release`) |
| **LZ4** | any | Development headers required |
| **Python 3** | ≥ 3.6 | For test data generation only |

### Install LZ4

```bash
# Ubuntu / Debian
sudo apt install liblz4-dev

# Fedora / RHEL
sudo dnf install lz4-devel

# macOS (Homebrew)
brew install lz4
```

### Build DuckDB (if not already built)

```bash
git clone https://github.com/duckdb/duckdb.git
cd duckdb
make release
cd ..
```

### Build the Extension

```bash
cd duckdb_tae_scanner
mkdir -p build && cd build

cmake .. \
  -DDUCKDB_DIR=/path/to/duckdb \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Release

make -j$(nproc)
```

The shared library is built as `build/libtae_scanner.so` (or `.dylib` on macOS).

#### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `DUCKDB_DIR` | *required* | Path to DuckDB source tree |
| `CMAKE_BUILD_TYPE` | `Debug` | `Release` for optimized build |
| `CMAKE_CXX_COMPILER` | system default | `clang++` recommended |
| `BUILD_TESTS` | `ON` | Set `OFF` to skip test target |

### Running Tests

```bash
# Generate test data (run from project root, not build/)
python3 test/gen_test_data.py

# Build and run all tests
cd build
make -j$(nproc) tae_tests
./test/tae_tests
```

#### Run tests by tag

```bash
./test/tae_tests "[scan]"           # Basic scan tests
./test/tae_tests "[filter]"         # Filter pushdown
./test/tae_tests "[filter_prune]"   # Filter-prune optimization
./test/tae_tests "[types]"          # Type mapping (decimal, uuid, blob)
./test/tae_tests "[datetime]"       # Date/timestamp epoch conversion
./test/tae_tests "[virtual]"        # Virtual columns
./test/tae_tests "[sampling]"       # Sampling pushdown
./test/tae_tests "[lz4]"            # LZ4 compression
./test/tae_tests "[order]"          # ORDER BY pushdown
./test/tae_tests "[partition_prune]" # Object-level pruning
./test/tae_tests "[stats]"          # Planner statistics
./test/tae_tests "[error]"          # Error handling
```

### Loading in DuckDB CLI

```bash
# Start DuckDB
./duckdb

# Load the extension
LOAD '/path/to/duckdb_tae_scanner/build/libtae_scanner.so';

# Query MO data
SELECT COUNT(*) FROM tae_scan('/path/to/manifest.json');
```

## Type Support

| MO Type | DuckDB Type | Fill Strategy |
|---------|-------------|---------------|
| bool, int8–int64, uint8–uint64, float32/64 | Native types | `memcpy` |
| decimal64 | DECIMAL(w,s) | `memcpy` (8 bytes) |
| decimal128 | DECIMAL(w,s) | `memcpy` (16 bytes) |
| date | DATE | Epoch offset adjustment |
| datetime, timestamp | TIMESTAMP | Epoch offset adjustment |
| char, varchar, text, json | VARCHAR | Varlena decode |
| blob, binary, varbinary | BLOB | Varlena decode |
| uuid | UUID | Big-endian → `hugeint_t` via `UUID::FromBlob` |
| enum | USMALLINT | `memcpy` |
| bit | UBIGINT | `memcpy` |

All types support filter pushdown (zone map + per-row), null bitmaps, CONSTANT vectors, and planner statistics.

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
| `columns[].width` | decimal only | Decimal precision |
| `columns[].scale` | decimal only | Decimal scale |
| `objects[].path` | yes | Object file path (relative to `data_dir` or manifest dir) |
| `objects[].rows` | yes | Total row count |
| `objects[].blocks` | yes | Block count |
| `data_dir` | no | Base directory for object paths (default: manifest's directory) |
| `sort_column` | no | Column name for ORDER BY pushdown hint |

Generate manifests from a running MatrixOne instance:

```bash
python3 tae_manifest_gen.py --db tpch --table lineitem --output manifest.json
```

## Architecture

See [DESIGN.md](DESIGN.md) for the full architecture document covering:
- TAE binary format specification (header, metadata, column layout, zone maps)
- 3-phase Init design (read_seqnums → output_map → filter extraction)
- Zone map evaluation and filter pipeline
- Object-level partition pruning
- ORDER BY pushdown with zone map sort keys
- S3 support and read coalescing
- posix_fadvise prefetching
- MO → Sirius-DB integration architecture

## License

Apache License 2.0 — see [LICENSE](LICENSE)
