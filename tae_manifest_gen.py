#!/usr/bin/env python3
"""
tae_manifest_gen.py — Generate a TAE manifest JSON for the DuckDB TAE scanner.

Connects to MatrixOne via MySQL protocol, discovers table schema and object list,
and writes a manifest file that tae_scan() can read.

Usage:
    python3 tae_manifest_gen.py --host 127.0.0.1 --port 6001 \
        --db tpch --table lineitem \
        --data-dir mo-data/shared \
        --output lineitem_manifest.json

    # With explicit flush before manifest generation:
    python3 tae_manifest_gen.py --host 127.0.0.1 --port 6001 \
        --db tpch --table lineitem \
        --data-dir mo-data/shared \
        --output lineitem_manifest.json --flush

Requirements:
    pip install pymysql
"""

import argparse
import base64
import json
import struct
import sys
from pathlib import Path

try:
    import pymysql
except ImportError:
    print("Error: pymysql not installed. Run: pip install pymysql", file=sys.stderr)
    sys.exit(1)


# ---------------------------------------------------------------------------
# MO Type OID mapping (from pkg/container/types/types.go)
# ---------------------------------------------------------------------------
MO_TYPE_MAP = {
    # SQL type string -> (oid, default_width)
    "BOOL":       (10, 1),
    "BOOLEAN":    (10, 1),
    "TINYINT":    (20, 1),
    "INT8":       (20, 1),
    "SMALLINT":   (21, 2),
    "INT16":      (21, 2),
    "INT":        (22, 4),
    "INTEGER":    (22, 4),
    "INT32":      (22, 4),
    "BIGINT":     (23, 8),
    "INT64":      (23, 8),
    "TINYINT UNSIGNED":  (25, 1),
    "UINT8":      (25, 1),
    "SMALLINT UNSIGNED": (26, 2),
    "UINT16":     (26, 2),
    "INT UNSIGNED":      (27, 4),
    "INTEGER UNSIGNED":  (27, 4),
    "UINT32":     (27, 4),
    "BIGINT UNSIGNED":   (28, 8),
    "UINT64":     (28, 8),
    "FLOAT":      (30, 4),
    "FLOAT32":    (30, 4),
    "DOUBLE":     (31, 8),
    "FLOAT64":    (31, 8),
    "CHAR":       (40, 24),
    "VARCHAR":    (41, 24),
    "BLOB":       (43, 24),
    "TEXT":       (44, 24),
    "BINARY":     (46, 24),
    "VARBINARY":  (47, 24),
    "DATALINK":   (48, 24),
    "DATE":       (50, 4),
    "TIME":       (51, 8),
    "DATETIME":   (52, 8),
    "TIMESTAMP":  (53, 8),
    "DECIMAL":    (60, 8),   # decimal64 by default, decimal128 for large precision
    "DECIMAL64":  (60, 8),
    "DECIMAL128": (61, 16),
    "DECIMAL256": (62, 32),
    "UUID":       (100, 16),
    "JSON":       (201, 24),
    "ENUM":       (200, 2),
    "BIT":        (210, 8),
}


def parse_sql_type(type_str: str) -> dict:
    """Parse a SQL type string like 'INT', 'VARCHAR(255)', 'DECIMAL(10,2)' into manifest format."""
    type_str = type_str.strip().upper()

    # Extract parameters: VARCHAR(255), DECIMAL(10,2), CHAR(32)
    width = 0
    scale = 0
    base_type = type_str

    paren_pos = type_str.find("(")
    if paren_pos >= 0:
        base_type = type_str[:paren_pos].strip()
        params = type_str[paren_pos + 1 : type_str.rfind(")")].strip()
        parts = [p.strip() for p in params.split(",")]
        if len(parts) >= 1 and parts[0]:
            width = int(parts[0])
        if len(parts) >= 2 and parts[1]:
            scale = int(parts[1])

    # Handle UNSIGNED suffix
    if "UNSIGNED" in type_str and base_type not in MO_TYPE_MAP:
        # e.g., "INT UNSIGNED" → already in map
        unsigned_key = base_type + " UNSIGNED"
        if unsigned_key in MO_TYPE_MAP:
            base_type = unsigned_key

    if base_type not in MO_TYPE_MAP:
        print(f"Warning: unknown type '{type_str}', defaulting to VARCHAR", file=sys.stderr)
        oid, default_width = 41, 24
    else:
        oid, default_width = MO_TYPE_MAP[base_type]

    # For DECIMAL, decide between decimal64 and decimal128
    if base_type == "DECIMAL" and width > 18:
        oid = 61  # decimal128
        default_width = 16

    return {
        "oid": oid,
        "width": width if width > 0 else default_width,
        "scale": scale,
    }


# ---------------------------------------------------------------------------
# ObjectStats decoder (154 bytes, from pkg/objectio/object_stats.go)
# ---------------------------------------------------------------------------
OBJECT_NAME_LEN = 60
NAME_STRING_OFF = 18
NAME_STRING_LEN = 42
EXTENT_LEN = 13
ROW_CNT_OFFSET = 73
BLK_CNT_OFFSET = 77
ZONE_MAP_OFFSET = 81
ZONE_MAP_LEN = 64
OBJ_SIZE_OFFSET = 145
OBJ_ORIGIN_SIZE_OFFSET = 149
OBJECT_STATS_LEN = 154


def decode_object_stats(b64_str: str) -> dict:
    """Decode a base64-encoded ObjectStats (154 bytes) into a dict."""
    raw = base64.b64decode(b64_str)
    if len(raw) < OBJECT_STATS_LEN:
        raise ValueError(f"ObjectStats too short: {len(raw)} < {OBJECT_STATS_LEN}")

    # Object name: 42-char string at offset 18
    name_bytes = raw[NAME_STRING_OFF : NAME_STRING_OFF + NAME_STRING_LEN]
    obj_name = name_bytes.decode("ascii").rstrip("\x00")

    # Row count: uint32 LE at offset 73
    row_cnt = struct.unpack_from("<I", raw, ROW_CNT_OFFSET)[0]

    # Block count: uint32 LE at offset 77
    blk_cnt = struct.unpack_from("<I", raw, BLK_CNT_OFFSET)[0]

    # Compressed size: uint32 LE at offset 145
    size = struct.unpack_from("<I", raw, OBJ_SIZE_OFFSET)[0]

    # Original size: uint32 LE at offset 149
    origin_size = struct.unpack_from("<I", raw, OBJ_ORIGIN_SIZE_OFFSET)[0]

    # Sort key zone map: 64 bytes at offset 81 (hex-encoded for manifest)
    zm_bytes = raw[ZONE_MAP_OFFSET : ZONE_MAP_OFFSET + ZONE_MAP_LEN]
    # All-zero zone map means no zone map data (object has no sort key or wasn't compacted)
    zone_map_hex = zm_bytes.hex() if any(b != 0 for b in zm_bytes) else ""

    result = {
        "path": obj_name,
        "rows": row_cnt,
        "blocks": blk_cnt,
        "size": size,
        "origin_size": origin_size,
    }
    if zone_map_hex:
        result["zone_map"] = zone_map_hex
    return result


# ---------------------------------------------------------------------------
# MO connection helpers
# ---------------------------------------------------------------------------

def connect_mo(host: str, port: int, user: str, password: str, db: str):
    """Connect to MatrixOne via MySQL protocol."""
    return pymysql.connect(
        host=host,
        port=port,
        user=user,
        password=password,
        database=db,
        charset="utf8mb4",
        connect_timeout=10,
    )


def get_table_schema(conn, db: str, table: str) -> list:
    """Get column schema using DESCRIBE."""
    columns = []
    with conn.cursor() as cur:
        cur.execute(f"DESCRIBE `{db}`.`{table}`")
        for row in cur.fetchall():
            col_name = row[0]
            col_type_str = row[1]
            is_nullable = row[2] == "YES"

            type_info = parse_sql_type(col_type_str)
            columns.append({
                "name": col_name,
                "type_str": col_type_str,
                "oid": type_info["oid"],
                "width": type_info["width"],
                "scale": type_info["scale"],
                "nullable": is_nullable,
            })
    return columns


def get_objects_via_inspect(conn, db: str, table: str) -> list:
    """Get object list via mo_ctl inspect with PPL4 verbosity."""
    objects = []
    with conn.cursor() as cur:
        sql = f"SELECT mo_ctl('dn', 'inspect', 'object -t {db}.{table} -vvvv')"
        cur.execute(sql)
        result = cur.fetchone()
        if not result or not result[0]:
            return objects

        output = result[0]
        if isinstance(output, bytes):
            output = output.decode("utf-8", errors="replace")

        # PPL4 output format (one per line):
        #   DATA
        #   <ShortStringEx> <base64_ObjectStats>
        #   ...
        #   summary: ...
        #
        #   TOMBSTONES
        #   <ShortStringEx> <base64_ObjectStats>
        #   ...
        #   summary: ...

        in_data_section = False
        for line in output.split("\n"):
            line = line.strip()
            if line == "DATA":
                in_data_section = True
                continue
            if line == "TOMBSTONES":
                in_data_section = False
                continue
            if line.startswith("summary:") or not line:
                continue
            if not in_data_section:
                continue

            # Parse: <short_id> <base64>
            parts = line.split(None, 1)
            if len(parts) != 2:
                continue

            short_id, b64_data = parts
            try:
                obj = decode_object_stats(b64_data)
                if obj["rows"] > 0:
                    objects.append(obj)
            except Exception as e:
                print(f"Warning: failed to decode object {short_id}: {e}", file=sys.stderr)

    return objects


def flush_table(conn, db: str, table: str):
    """Force flush table data from memory to disk."""
    with conn.cursor() as cur:
        sql = f"SELECT mo_ctl('dn', 'flush', '{db}.{table}')"
        cur.execute(sql)
        result = cur.fetchone()
        print(f"Flush result: {result[0] if result else 'OK'}", file=sys.stderr)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Generate TAE manifest for DuckDB TAE scanner extension"
    )
    parser.add_argument("--host", default="127.0.0.1", help="MO host (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=6001, help="MO port (default: 6001)")
    parser.add_argument("--user", default="root", help="MO user (default: root)")
    parser.add_argument("--password", default="111", help="MO password (default: 111)")
    parser.add_argument("--db", required=True, help="Database name")
    parser.add_argument("--table", required=True, help="Table name")
    parser.add_argument("--data-dir", required=True, help="MO shared fileservice root (e.g. mo-data/shared)")
    parser.add_argument("--sort-column", default=None, help="Sort key column name (enables zone_map fast path)")
    parser.add_argument("--output", "-o", required=True, help="Output manifest JSON path")
    parser.add_argument("--flush", action="store_true", help="Flush table before generating manifest")
    parser.add_argument("--pretty", action="store_true", help="Pretty-print JSON output")
    args = parser.parse_args()

    conn = connect_mo(args.host, args.port, args.user, args.password, args.db)
    try:
        if args.flush:
            print(f"Flushing {args.db}.{args.table}...", file=sys.stderr)
            flush_table(conn, args.db, args.table)

        print(f"Discovering schema for {args.db}.{args.table}...", file=sys.stderr)
        columns = get_table_schema(conn, args.db, args.table)
        print(f"  Found {len(columns)} columns", file=sys.stderr)

        print(f"Discovering objects for {args.db}.{args.table}...", file=sys.stderr)
        objects = get_objects_via_inspect(conn, args.db, args.table)
        print(f"  Found {len(objects)} data objects", file=sys.stderr)

        total_rows = sum(o["rows"] for o in objects)
        total_size = sum(o["origin_size"] for o in objects)
        print(f"  Total: {total_rows:,} rows, {total_size:,} bytes ({total_size/1024/1024:.1f} MB)",
              file=sys.stderr)

        manifest = {
            "version": 1,
            "database": args.db,
            "table": args.table,
            "data_dir": str(Path(args.data_dir).resolve()),
        }
        if args.sort_column:
            # Validate sort_column exists in schema
            col_names = [c["name"] for c in columns]
            if args.sort_column not in col_names:
                print(f"Warning: --sort-column '{args.sort_column}' not found in table columns: {col_names}",
                      file=sys.stderr)
            manifest["sort_column"] = args.sort_column
            # Count objects with zone maps
            zm_count = sum(1 for o in objects if "zone_map" in o)
            print(f"  {zm_count}/{len(objects)} objects have zone maps", file=sys.stderr)
        manifest["columns"] = [
            {
                "name": c["name"],
                "type_str": c["type_str"],
                "oid": c["oid"],
                "width": c["width"],
                "scale": c["scale"],
                "nullable": c["nullable"],
            }
            for c in columns
        ]
        manifest["objects"] = objects
        manifest["stats"] = {
            "total_rows": total_rows,
            "total_objects": len(objects),
            "total_origin_size": total_size,
        }

        output_path = Path(args.output)
        with open(output_path, "w") as f:
            if args.pretty:
                json.dump(manifest, f, indent=2)
            else:
                json.dump(manifest, f)

        print(f"Manifest written to {output_path}", file=sys.stderr)
        print(f"Use in DuckDB: SELECT * FROM tae_scan('{output_path}');", file=sys.stderr)

    finally:
        conn.close()


if __name__ == "__main__":
    main()
