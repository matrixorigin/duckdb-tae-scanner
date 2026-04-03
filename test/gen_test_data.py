#!/usr/bin/env python3
"""Generate synthetic TAE object files for testing the DuckDB TAE scanner.

Each generated file contains known data so that the C++ reader tests can
verify correctness of header parsing, metadata extraction, column decoding,
zone map evaluation, and null bitmap handling.

Usage:
    python gen_test_data.py [output_dir]

Generates:
    basic_3col.tae       — 1 block, 3 columns (int32, varchar, float64), 8 rows
    multi_block.tae      — 2 blocks, 2 columns (int32, bool)
    with_nulls.tae       — 1 block, 2 columns (int32, varchar) with nulls
    manifest.json        — Manifest referencing all generated files
"""

import json
import os
import struct
import sys

# --------------------------------------------------------------------------
# Constants — must match tae_object_reader.hpp exactly
# --------------------------------------------------------------------------
OBJECT_MAGIC        = 0xFFFFFFFF
OBJECT_VERSION      = 1
HEADER_SIZE         = 64
HEADER_META_OFF     = 10

META_V3_HEADER_LEN  = 32
BLOCK_HEADER_SIZE   = 179
COL_META_LEN        = 124
IO_ENTRY_HEADER_LEN = 4

# IOEntryHeader types
IOET_OBJ_META = 1
IOET_COL_DATA = 2

# BlockHeader field offsets
BH_ROWS_OFF         = 36
BH_COL_COUNT_OFF    = 40
BH_META_COL_CNT_OFF = 140
BH_MAX_SEQ_OFF      = 142

# ColumnMeta field offsets
CM_DATA_TYPE_OFF = 0
CM_IDX_OFF       = 1
CM_NDV_OFF       = 3
CM_NULL_CNT_OFF  = 7
CM_LOCATION_OFF  = 11
CM_CHECKSUM_OFF  = 24
CM_ZONEMAP_OFF   = 28

# Zone map offsets
ZM_SIZE         = 64
ZM_MIN_OFF      = 0
ZM_MIN_LEN_OFF  = 30
ZM_MAX_OFF      = 31
ZM_MAX_INFO_OFF = 61
ZM_SCALE_OFF    = 62
ZM_TYPE_OFF     = 63
ZM_INIT_MASK    = 0x80

# MO Type OIDs
MO_T_BOOL    = 10
MO_T_INT32   = 22
MO_T_FLOAT64 = 31
MO_T_VARCHAR = 61

# Varlena
VARLENA_SIZE       = 24
VARLENA_INLINE_MAX = 23
VARLENA_BIG_MARKER = 0xFFFFFFFF


# --------------------------------------------------------------------------
# Low-level binary builders
# --------------------------------------------------------------------------

def pack_extent(alg, offset, length, origin_size):
    """Pack a 13-byte Extent."""
    return struct.pack('<BIII', alg, offset, length, origin_size)


def pack_io_entry_header(entry_type, version=0):
    """Pack a 4-byte IOEntryHeader."""
    return struct.pack('<HH', entry_type, version)


def pack_mo_type(oid, size, width=0, scale=0):
    """Pack a 16-byte MOType struct (native LE)."""
    # oid(1) charset(1) not_null(1) dummy(1) size(i4) width(i4) scale(i4)
    return struct.pack('<BBBBiii', oid, 0, 0, 0, size, width, scale)


def build_zone_map_numeric(mo_type_oid, min_val, max_val, elem_size):
    """Build a 64-byte zone map for a fixed-width numeric type."""
    zm = bytearray(ZM_SIZE)
    # Pack min value
    if elem_size == 1:
        struct.pack_into('<b', zm, ZM_MIN_OFF, min_val)
    elif elem_size == 4:
        if mo_type_oid == MO_T_FLOAT64:
            # shouldn't happen for 4-byte but handle gracefully
            struct.pack_into('<f', zm, ZM_MIN_OFF, min_val)
        else:
            struct.pack_into('<i', zm, ZM_MIN_OFF, min_val)
    elif elem_size == 8:
        if mo_type_oid == MO_T_FLOAT64:
            struct.pack_into('<d', zm, ZM_MIN_OFF, min_val)
        else:
            struct.pack_into('<q', zm, ZM_MIN_OFF, min_val)
    zm[ZM_MIN_LEN_OFF] = min(elem_size, 30)  # min length info

    # Pack max value
    if elem_size == 1:
        struct.pack_into('<b', zm, ZM_MAX_OFF, max_val)
    elif elem_size == 4:
        if mo_type_oid == MO_T_FLOAT64:
            struct.pack_into('<f', zm, ZM_MAX_OFF, max_val)
        else:
            struct.pack_into('<i', zm, ZM_MAX_OFF, max_val)
    elif elem_size == 8:
        if mo_type_oid == MO_T_FLOAT64:
            struct.pack_into('<d', zm, ZM_MAX_OFF, max_val)
        else:
            struct.pack_into('<q', zm, ZM_MAX_OFF, max_val)
    zm[ZM_MAX_INFO_OFF] = min(elem_size, 30)  # max length info

    zm[ZM_SCALE_OFF] = ZM_INIT_MASK  # initialized
    zm[ZM_TYPE_OFF] = mo_type_oid
    return bytes(zm)


def build_zone_map_string(mo_type_oid, min_str, max_str):
    """Build a 64-byte zone map for a string/varchar type."""
    zm = bytearray(ZM_SIZE)
    # Min: copy up to 30 bytes
    min_bytes = min_str.encode('utf-8')[:30]
    zm[ZM_MIN_OFF:ZM_MIN_OFF + len(min_bytes)] = min_bytes
    zm[ZM_MIN_LEN_OFF] = len(min_bytes)

    # Max: copy up to 30 bytes
    max_bytes = max_str.encode('utf-8')[:30]
    zm[ZM_MAX_OFF:ZM_MAX_OFF + len(max_bytes)] = max_bytes
    zm[ZM_MAX_INFO_OFF] = len(max_bytes)

    zm[ZM_SCALE_OFF] = ZM_INIT_MASK
    zm[ZM_TYPE_OFF] = mo_type_oid
    return bytes(zm)


def build_null_bitmap(nulls, row_count):
    """Build null bitmap section.
    nulls: list of bool, True = null.
    Returns bytes for the nsp section.
    """
    null_count = sum(nulls)
    if null_count == 0:
        return b''  # nspLen = 0

    # Number of uint64 words needed
    num_words = (row_count + 63) // 64
    data_size = num_words * 8
    bitmap = bytearray(data_size)

    for i, is_null in enumerate(nulls):
        if is_null:
            word_idx = i // 64
            bit_idx = i % 64
            existing = struct.unpack_from('<Q', bitmap, word_idx * 8)[0]
            existing |= (1 << bit_idx)
            struct.pack_into('<Q', bitmap, word_idx * 8, existing)

    # Header: count(i64) + bitmap_len(u64) + data_size(u64)
    header = struct.pack('<qQQ', null_count, num_words, data_size)
    return header + bytes(bitmap)


def encode_varlena(s):
    """Encode a string as a 24-byte varlena slot (inline only for test data)."""
    data = s.encode('utf-8')
    slot = bytearray(VARLENA_SIZE)
    if len(data) <= VARLENA_INLINE_MAX:
        slot[0] = len(data)
        slot[1:1 + len(data)] = data
    else:
        raise ValueError(f"String too long for inline varlena: {len(data)} > {VARLENA_INLINE_MAX}")
    return bytes(slot)


def build_vector(mo_type_oid, elem_size, values, nulls=None, is_varchar=False):
    """Build a complete MO vector binary (what goes after IOEntryHeader).

    For varchar: values is list of str, elem_size is -24 (VARLENA_SIZE marker).
    For numeric: values is list of int/float, elem_size is the byte width.
    """
    row_count = len(values)
    if nulls is None:
        nulls = [False] * row_count

    # Build the data section
    if is_varchar:
        area_buf = bytearray()
        data_buf = bytearray()
        for v in values:
            s = v if v is not None else ''
            encoded = s.encode('utf-8')
            if len(encoded) <= VARLENA_INLINE_MAX:
                slot = bytearray(VARLENA_SIZE)
                slot[0] = len(encoded)
                slot[1:1 + len(encoded)] = encoded
                data_buf += slot
            else:
                # Big varlena: reference into area buffer
                slot = bytearray(VARLENA_SIZE)
                struct.pack_into('<I', slot, 0, VARLENA_BIG_MARKER)
                struct.pack_into('<I', slot, 4, len(area_buf))
                struct.pack_into('<I', slot, 8, len(encoded))
                data_buf += slot
                area_buf += encoded
        data_bytes = bytes(data_buf)
        area_bytes = bytes(area_buf)
        actual_elem_size = -24  # varlena marker
    else:
        if mo_type_oid == MO_T_BOOL:
            data_bytes = bytes(1 if v else 0 for v in values)
        elif mo_type_oid == MO_T_INT32:
            data_bytes = b''.join(struct.pack('<i', v if v is not None else 0) for v in values)
        elif mo_type_oid == MO_T_FLOAT64:
            data_bytes = b''.join(struct.pack('<d', v if v is not None else 0.0) for v in values)
        else:
            raise ValueError(f"unsupported type OID {mo_type_oid}")
        area_bytes = b''
        actual_elem_size = elem_size

    # Null bitmap
    nsp_bytes = build_null_bitmap(nulls, row_count)

    # Assemble vector binary
    buf = bytearray()
    buf += struct.pack('<B', 1)  # class = FLAT
    buf += pack_mo_type(mo_type_oid, actual_elem_size)  # 16 bytes
    buf += struct.pack('<I', row_count)  # length
    buf += struct.pack('<I', len(data_bytes))  # dataLen
    buf += data_bytes
    buf += struct.pack('<I', len(area_bytes))  # areaLen
    buf += area_bytes
    buf += struct.pack('<I', len(nsp_bytes))  # nspLen
    buf += nsp_bytes
    buf += struct.pack('<B', 0)  # sorted = false

    return bytes(buf)


def build_col_data_extent(vector_bytes):
    """Wrap vector bytes with IOEntryHeader to form column data block."""
    return pack_io_entry_header(IOET_COL_DATA) + vector_bytes


# --------------------------------------------------------------------------
# Block metadata builder
# --------------------------------------------------------------------------

def build_block_header(rows, col_count, meta_col_count=None, max_seq=None):
    """Build a 179-byte BlockHeader."""
    if meta_col_count is None:
        meta_col_count = col_count
    if max_seq is None:
        max_seq = col_count - 1

    hdr = bytearray(BLOCK_HEADER_SIZE)
    # dbID (8), tableID (8), blockID (20) = zeros
    struct.pack_into('<I', hdr, BH_ROWS_OFF, rows)
    struct.pack_into('<H', hdr, BH_COL_COUNT_OFF, col_count)
    struct.pack_into('<H', hdr, BH_META_COL_CNT_OFF, meta_col_count)
    struct.pack_into('<H', hdr, BH_MAX_SEQ_OFF, max_seq)
    return bytes(hdr)


def build_col_meta(data_type, idx, ndv, null_cnt, location_extent, zone_map_bytes):
    """Build a 124-byte ColumnMeta."""
    cm = bytearray(COL_META_LEN)
    cm[CM_DATA_TYPE_OFF] = data_type
    struct.pack_into('<H', cm, CM_IDX_OFF, idx)
    struct.pack_into('<I', cm, CM_NDV_OFF, ndv)
    struct.pack_into('<I', cm, CM_NULL_CNT_OFF, null_cnt)
    cm[CM_LOCATION_OFF:CM_LOCATION_OFF + 13] = location_extent
    # checksum = 0
    cm[CM_ZONEMAP_OFF:CM_ZONEMAP_OFF + ZM_SIZE] = zone_map_bytes
    return bytes(cm)


# --------------------------------------------------------------------------
# Full TAE file builder
# --------------------------------------------------------------------------

class TAEFileBuilder:
    """Builds a complete TAE object file."""

    def __init__(self):
        self.blocks = []   # list of BlockSpec
        self.buf = bytearray()

    def add_block(self, block_spec):
        self.blocks.append(block_spec)

    def build(self):
        """Assemble the complete file and return bytes."""
        self.buf = bytearray(HEADER_SIZE)  # placeholder header

        # Phase 1: write column data blocks, record extents
        for block in self.blocks:
            for col in block['columns']:
                col_data = build_col_data_extent(col['vector_bytes'])
                offset = len(self.buf)
                length = len(col_data)
                # No compression for test data
                extent = pack_extent(0, offset, length, length)
                col['extent'] = extent
                self.buf += col_data

        # Phase 2: build metadata
        meta_bytes = self._build_metadata()
        meta_offset = len(self.buf)
        meta_length = len(meta_bytes)
        self.buf += meta_bytes

        # Phase 3: fill in header
        struct.pack_into('<Q', self.buf, 0, OBJECT_MAGIC)       # magic
        struct.pack_into('<H', self.buf, 8, OBJECT_VERSION)     # version
        # Meta extent at offset 10 (no compression)
        meta_extent = pack_extent(0, meta_offset, meta_length, meta_length)
        self.buf[HEADER_META_OFF:HEADER_META_OFF + 13] = meta_extent

        return bytes(self.buf)

    def _build_metadata(self):
        """Build the complete metadata section."""
        # IOEntryHeader
        meta = bytearray()
        meta += pack_io_entry_header(IOET_OBJ_META)

        # objectMetaV3 header (32 bytes)
        v3_header = bytearray(META_V3_HEADER_LEN)
        struct.pack_into('<H', v3_header, 0, 1)    # dataMetaCount = 1
        struct.pack_into('<I', v3_header, 2, META_V3_HEADER_LEN)  # dataMetaOffset (after this header)
        meta += v3_header

        # DataMeta starts here
        dm_start = len(meta) - IO_ENTRY_HEADER_LEN  # relative to after IOEntryHeader
        # Wait, the offset in v3 header is relative to buffer after IOEntryHeader stripping.
        # Let me re-check: ParseMetadata receives buf + IO_ENTRY_HEADER_LEN.
        # So data_meta_offset is relative to the buffer passed to ParseMetadata.
        # That buffer starts right after the IOEntryHeader.
        # v3_header itself is the first 32 bytes of that buffer.
        # data_meta_offset = META_V3_HEADER_LEN means DataMeta starts right after v3 header.

        # Object-level BlockHeader (summarizes all blocks)
        total_rows = sum(b['rows'] for b in self.blocks)
        col_count = len(self.blocks[0]['columns'])
        obj_block_header = build_block_header(total_rows, col_count)
        meta += obj_block_header

        # Object-level ColumnMeta (one per column, can be zeros for now)
        for c in range(col_count):
            col = self.blocks[0]['columns'][c]
            zm = col.get('zone_map', bytes(ZM_SIZE))
            cm = build_col_meta(col['mo_type'], c, 0, 0, bytes(13), zm)
            meta += cm

        # BlockIndex
        # We'll compute block offsets relative to DataMeta start.
        # DataMeta start = after IOEntryHeader + v3_header, i.e., offset META_V3_HEADER_LEN
        # in the ParseMetadata buffer.
        #
        # dm = buf + data_meta_offset  (buf is after IOEntryHeader)
        # obj_data_len = BLOCK_HEADER_SIZE + obj_meta_col_count * COL_META_LEN
        # bi = dm + obj_data_len
        #
        # So BlockIndex follows right after obj-level BlockHeader + obj-level ColumnMetas.
        obj_data_len = BLOCK_HEADER_SIZE + col_count * COL_META_LEN

        # Build per-block metadata first to know sizes
        per_block_metas = []
        for block in self.blocks:
            ncols = len(block['columns'])
            bh = build_block_header(block['rows'], ncols)
            cms = b''
            for i, col in enumerate(block['columns']):
                null_cnt = sum(1 for n in col.get('nulls', []) if n)
                zm = col.get('zone_map', bytes(ZM_SIZE))
                cm = build_col_meta(col['mo_type'], i, col.get('ndv', 0),
                                    null_cnt, col['extent'], zm)
                cms += cm
            per_block_metas.append(bh + cms)

        # BlockIndex: block_count(4B) + [offset(4B) + length(4B)] × block_count
        block_count = len(self.blocks)
        bi_size = 4 + block_count * 8

        # Per-block data follows immediately after BlockIndex
        # Offsets are relative to dm (= DataMeta start)
        block_data_start = obj_data_len + bi_size
        bi = struct.pack('<I', block_count)
        current_offset = block_data_start
        for pbm in per_block_metas:
            bi += struct.pack('<II', current_offset, len(pbm))
            current_offset += len(pbm)
        meta += bi

        # Append per-block metadata
        for pbm in per_block_metas:
            meta += pbm

        return bytes(meta)


# --------------------------------------------------------------------------
# Test file generators
# --------------------------------------------------------------------------

def gen_basic_3col(outdir):
    """1 block, 3 columns (int32, varchar, float64), 8 rows, no nulls."""
    int_vals  = [10, 20, 30, 40, 50, 60, 70, 80]
    str_vals  = ['alpha', 'beta', 'gamma', 'delta', 'epsilon', 'zeta', 'eta', 'theta']
    dbl_vals  = [1.1, 2.2, 3.3, 4.4, 5.5, 6.6, 7.7, 8.8]

    builder = TAEFileBuilder()
    builder.add_block({
        'rows': 8,
        'columns': [
            {
                'mo_type': MO_T_INT32,
                'vector_bytes': build_vector(MO_T_INT32, 4, int_vals),
                'zone_map': build_zone_map_numeric(MO_T_INT32, 10, 80, 4),
                'ndv': 8,
            },
            {
                'mo_type': MO_T_VARCHAR,
                'vector_bytes': build_vector(MO_T_VARCHAR, -24, str_vals, is_varchar=True),
                'zone_map': build_zone_map_string(MO_T_VARCHAR, 'alpha', 'zeta'),
                'ndv': 8,
            },
            {
                'mo_type': MO_T_FLOAT64,
                'vector_bytes': build_vector(MO_T_FLOAT64, 8, dbl_vals),
                'zone_map': build_zone_map_numeric(MO_T_FLOAT64, 1.1, 8.8, 8),
                'ndv': 8,
            },
        ],
    })

    path = os.path.join(outdir, 'basic_3col.tae')
    with open(path, 'wb') as f:
        f.write(builder.build())
    return path, int_vals, str_vals, dbl_vals


def gen_multi_block(outdir):
    """2 blocks, 2 columns (int32, bool)."""
    builder = TAEFileBuilder()

    # Block 0: int32 = [1,2,3,4], bool = [T,F,T,F]
    builder.add_block({
        'rows': 4,
        'columns': [
            {
                'mo_type': MO_T_INT32,
                'vector_bytes': build_vector(MO_T_INT32, 4, [1, 2, 3, 4]),
                'zone_map': build_zone_map_numeric(MO_T_INT32, 1, 4, 4),
                'ndv': 4,
            },
            {
                'mo_type': MO_T_BOOL,
                'vector_bytes': build_vector(MO_T_BOOL, 1, [True, False, True, False]),
                'zone_map': build_zone_map_numeric(MO_T_BOOL, 0, 1, 1),
                'ndv': 2,
            },
        ],
    })

    # Block 1: int32 = [100,200,300,400], bool = [F,F,T,T]
    builder.add_block({
        'rows': 4,
        'columns': [
            {
                'mo_type': MO_T_INT32,
                'vector_bytes': build_vector(MO_T_INT32, 4, [100, 200, 300, 400]),
                'zone_map': build_zone_map_numeric(MO_T_INT32, 100, 400, 4),
                'ndv': 4,
            },
            {
                'mo_type': MO_T_BOOL,
                'vector_bytes': build_vector(MO_T_BOOL, 1, [False, False, True, True]),
                'zone_map': build_zone_map_numeric(MO_T_BOOL, 0, 1, 1),
                'ndv': 2,
            },
        ],
    })

    path = os.path.join(outdir, 'multi_block.tae')
    with open(path, 'wb') as f:
        f.write(builder.build())
    return path


def gen_with_nulls(outdir):
    """1 block, 2 columns (int32, varchar), 6 rows, with nulls."""
    int_vals  = [10, None, 30, None, 50, 60]
    int_nulls = [False, True, False, True, False, False]
    str_vals  = ['hello', 'world', None, 'test', None, 'done']
    str_nulls = [False, False, True, False, True, False]

    builder = TAEFileBuilder()
    builder.add_block({
        'rows': 6,
        'columns': [
            {
                'mo_type': MO_T_INT32,
                'vector_bytes': build_vector(MO_T_INT32, 4, int_vals, nulls=int_nulls),
                'zone_map': build_zone_map_numeric(MO_T_INT32, 10, 60, 4),
                'nulls': int_nulls,
                'ndv': 4,
            },
            {
                'mo_type': MO_T_VARCHAR,
                'vector_bytes': build_vector(MO_T_VARCHAR, -24, str_vals,
                                             nulls=str_nulls, is_varchar=True),
                'zone_map': build_zone_map_string(MO_T_VARCHAR, 'done', 'world'),
                'nulls': str_nulls,
                'ndv': 4,
            },
        ],
    })

    path = os.path.join(outdir, 'with_nulls.tae')
    with open(path, 'wb') as f:
        f.write(builder.build())
    return path


def gen_basic_3col_part2(outdir):
    """Second 3-column file for multi-file testing: different data, same schema."""
    int_vals  = [100, 200, 300, 400]
    str_vals  = ['one', 'two', 'three', 'four']
    dbl_vals  = [10.1, 20.2, 30.3, 40.4]

    builder = TAEFileBuilder()
    builder.add_block({
        'rows': 4,
        'columns': [
            {
                'mo_type': MO_T_INT32,
                'vector_bytes': build_vector(MO_T_INT32, 4, int_vals),
                'zone_map': build_zone_map_numeric(MO_T_INT32, 100, 400, 4),
                'ndv': 4,
            },
            {
                'mo_type': MO_T_VARCHAR,
                'vector_bytes': build_vector(MO_T_VARCHAR, -24, str_vals, is_varchar=True),
                'zone_map': build_zone_map_string(MO_T_VARCHAR, 'four', 'two'),
                'ndv': 4,
            },
            {
                'mo_type': MO_T_FLOAT64,
                'vector_bytes': build_vector(MO_T_FLOAT64, 8, dbl_vals),
                'zone_map': build_zone_map_numeric(MO_T_FLOAT64, 10.1, 40.4, 8),
                'ndv': 4,
            },
        ],
    })

    path = os.path.join(outdir, 'basic_3col_part2.tae')
    with open(path, 'wb') as f:
        f.write(builder.build())
    return path


def gen_manifest(outdir, files):
    """Generate manifest JSON for the test data."""
    manifest = {
        'database': 'test_db',
        'table': 'test_table',
        'columns': [
            {'name': 'col_int', 'oid': MO_T_INT32},
            {'name': 'col_str', 'oid': MO_T_VARCHAR},
            {'name': 'col_dbl', 'oid': MO_T_FLOAT64},
        ],
        'objects': [],
    }
    for f in files:
        fname = os.path.basename(f)
        manifest['objects'].append({
            'path': fname,
            'rows': 8,
            'blocks': 1,
            'size': os.path.getsize(f),
        })

    path = os.path.join(outdir, 'manifest.json')
    with open(path, 'w') as f:
        json.dump(manifest, f, indent=2)
    return path


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else 'test/data'
    os.makedirs(outdir, exist_ok=True)

    basic_path, _, _, _ = gen_basic_3col(outdir)
    multi_path = gen_multi_block(outdir)
    nulls_path = gen_with_nulls(outdir)
    part2_path = gen_basic_3col_part2(outdir)

    gen_manifest(outdir, [basic_path])

    # Multi-file manifest: two .tae files with same 3-column schema
    multifile_manifest = {
        'database': 'test_db',
        'table': 'test_multifile',
        'columns': [
            {'name': 'col_int', 'oid': MO_T_INT32},
            {'name': 'col_str', 'oid': MO_T_VARCHAR},
            {'name': 'col_dbl', 'oid': MO_T_FLOAT64},
        ],
        'objects': [
            {
                'path': os.path.basename(basic_path),
                'rows': 8,
                'blocks': 1,
                'size': os.path.getsize(basic_path),
            },
            {
                'path': os.path.basename(part2_path),
                'rows': 4,
                'blocks': 1,
                'size': os.path.getsize(part2_path),
            },
        ],
    }
    mf_path = os.path.join(outdir, 'manifest_multifile.json')
    with open(mf_path, 'w') as f:
        json.dump(multifile_manifest, f, indent=2)

    print(f'Generated test data in {outdir}/')
    print(f'  {os.path.basename(basic_path)}  ({os.path.getsize(basic_path)} bytes)')
    print(f'  {os.path.basename(part2_path)}  ({os.path.getsize(part2_path)} bytes)')
    print(f'  {os.path.basename(multi_path)}  ({os.path.getsize(multi_path)} bytes)')
    print(f'  {os.path.basename(nulls_path)}  ({os.path.getsize(nulls_path)} bytes)')
    print(f'  manifest.json')
    print(f'  manifest_multifile.json')


if __name__ == '__main__':
    main()
