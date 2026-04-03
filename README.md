# DuckDB TAE Scanner Extension

A DuckDB extension that reads [MatrixOne](https://github.com/matrixorigin/matrixone) TAE (Table Analytics Engine) object files directly, enabling external query engines to scan MO data without ETL.

## Features

- **Direct TAE binary format reading** — parses MO's columnar storage format (header, metadata, column data, zone maps)
- **DuckDB FileSystem abstraction** — transparent support for local files and S3 (`s3://`) paths
- **Projection pushdown** — reads only the columns needed by the query
- **Zone map block skipping** — evaluates pushed-down predicates against per-block zone maps to skip irrelevant blocks
- **LZ4 decompression** — handles MO's LZ4-compressed column data
- **Full MO type support** — maps all MO types (bool, int8–uint64, float32/64, decimal64/128, date, datetime, timestamp, char, varchar, blob, text, uuid, enum) to DuckDB logical types

## Usage

```sql
-- Load the extension
LOAD 'tae_scanner.so';

-- Scan a table via manifest file
SELECT * FROM tae_scan('/path/to/data', 'manifest.json');
```

The manifest JSON file describes the schema, object list, and data directory. Generate it using `tae_manifest_gen.py` against a running MatrixOne instance.

## Building

```bash
mkdir build && cd build
cmake -DDUCKDB_DIR=/path/to/duckdb ..
make -j$(nproc)
```

### Dependencies
- DuckDB (source tree or installed)
- LZ4 (`liblz4-dev`)
- C++17 compiler

## Architecture

See [DESIGN.md](DESIGN.md) for the full architecture document covering:
- TAE binary format specification
- Scan pipeline design
- Zone map evaluation
- S3 support and read coalescing
- MO → Sirius-DB integration architecture

## License

Apache License 2.0 — see [LICENSE](LICENSE)
