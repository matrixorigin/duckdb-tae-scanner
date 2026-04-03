# DuckDB TAE Scanner Extension

A DuckDB extension that reads [MatrixOne](https://github.com/matrixorigin/matrixone) TAE (Table Analytics Engine) object files directly, enabling external query engines to scan MO data without ETL.

## Features

- **Direct TAE binary format reading** — parses MO's columnar storage format (header, metadata, column data, zone maps)
- **DuckDB FileSystem abstraction** — transparent support for local files and S3 (`s3://`) paths
- **Projection pushdown** — reads only the columns needed by the query
- **Zone map block skipping** — evaluates pushed-down predicates against per-block zone maps to skip irrelevant blocks
- **Per-row filter evaluation** — applies pushed-down comparisons row-by-row after zone map block skip
- **LZ4 decompression** — handles MO's LZ4-compressed column data
- **Full MO type support** — maps all MO types (bool, int8–uint64, float32/64, decimal64/128, date, datetime, timestamp, char, varchar, blob, text, uuid, enum) to DuckDB logical types
- **CONSTANT vector support** — handles MO constant vectors (single value broadcast to all rows)
- **Virtual columns** — exposes `file_path` and `block_id` as virtual columns for provenance tracking
- **Sampling pushdown** — supports `TABLESAMPLE SYSTEM(N%)` via Bernoulli sampling in the scan
- **Parallel block scanning** — multi-threaded execution with atomic work dispatch
- **Planner statistics** — provides row-count estimates and column-level min/max from zone maps
- **EXPLAIN integration** — shows table name, object count, row count in query plans

## Usage

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

```bash
mkdir build && cd build
cmake -DDUCKDB_DIR=/path/to/duckdb ..
make -j$(nproc)
```

### Running Tests

```bash
# Generate test data (run from project root)
python3 test/gen_test_data.py

# Build and run tests
cd build && make -j$(nproc) tae_tests && ./test/tae_tests
```

### Dependencies
- DuckDB (source tree or installed)
- LZ4 (`liblz4-dev`)
- C++17 compiler

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

## Architecture

See [DESIGN.md](DESIGN.md) for the full architecture document covering:
- TAE binary format specification
- Scan pipeline design
- Zone map evaluation
- S3 support and read coalescing
- MO → Sirius-DB integration architecture

## License

Apache License 2.0 — see [LICENSE](LICENSE)
