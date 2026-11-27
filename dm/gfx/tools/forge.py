"""Ubisoft Anvil scimitar (.forge) archives and THFIRE Oodle blocks.

Each .forge starts with ``scimitar\\0``, a u32 format version (0x1c) and a u64
header offset; that header names an entry count and a 20-byte-per-entry index
table ``{u64 offset, u64 file_data_id, u32 stored_size}``. Entries hold either
raw bytes or one or more ``33 aa fb 57 99 fa 04 10`` (THFIRE) chunked blocks,
each carrying ``n`` chunks (preceded by 4-byte CRCs) with Oodle Mermaid/Selkie
(codec 10) or uncompressed (``raw == stored``) payloads. Used by shader_split.py
(forge_jobs, forge_shaders) for Immortals Fenyx Rising.
"""
import struct
from romfs import open

SCIMITAR_MAGIC = b'scimitar\x00'
THFIRE_MAGIC = bytes.fromhex('33aafb5799fa0410')
MAX_ENTRIES = 1_000_000
MAX_MEMBER = 256 << 20


def _range(offset, size, end, what):
    if offset < 0 or size < 0 or offset > end or size > end - offset:
        raise ValueError(f'{what}: range {offset:#x}+{size:#x} exceeds {end:#x} bytes')


def entries(path):
    """Archive entries as ``[(offset, file_data_id, stored_size), ...]`` sorted by offset,
    or ``None`` if *path* is not a scimitar .forge archive.
    """
    with open(path, 'rb') as f:
        f.seek(0, 2)
        size = f.tell()
        if size < 0x20:
            return None
        f.seek(0)
        head = f.read(0x20)
        if len(head) < 0x20 or head[:9] != SCIMITAR_MAGIC:
            return None
        ver = struct.unpack_from('<I', head, 9)[0]
        hdr_off = struct.unpack_from('<Q', head, 0x0d)[0]
        if ver != 0x1c:
            raise ValueError(f'{path}: unsupported scimitar .forge version {ver:#x}')
        _range(hdr_off, 0x40, size, f'{path}: scimitar header')
        f.seek(hdr_off)
        hdr = f.read(0x40)
        count = struct.unpack_from('<I', hdr, 0)[0]
        if count > MAX_ENTRIES:
            raise ValueError(f'{path}: invalid scimitar entry count {count}')
        if not count:
            return []
        table_off = struct.unpack_from('<Q', hdr, 0x34)[0]
        _range(table_off, count * 20, size, f'{path}: scimitar index table')
        f.seek(table_off)
        table = f.read(count * 20)
    if len(table) != count * 20:
        raise ValueError(f'{path}: truncated scimitar index table')
    out = []
    for i in range(count):
        off, fid, stored = struct.unpack_from('<QQI', table, i * 20)
        if not off or stored < 0x13:
            continue
        _range(off, stored, size, f'{path}: entry {fid:#x}')
        if stored > MAX_MEMBER:
            raise ValueError(f'{path}: entry {fid:#x} exceeds max size ({stored} bytes)')
        out.append((off, fid, stored))
    out.sort()
    return out


def needs_oodle(path):
    """Probe scimitar entries for compressed THFIRE Oodle (codec 10) chunks."""
    try:
        ents = entries(path)
        if not ents:
            return False
        with open(path, 'rb') as f:
            for off, _, stored in ents:
                if stored < 27:
                    continue
                f.seek(off)
                buf = f.read(min(stored, 0x100))
                pos = 0
                while True:
                    p = buf.find(THFIRE_MAGIC, pos)
                    if p < 0 or p + 19 > len(buf):
                        break
                    ver, comp, _, n = struct.unpack_from('<HBII', buf, p + 8)
                    cur = p + 19
                    if ver == 3 and 1 <= n <= 100000 and cur + n * 8 <= len(buf):
                        for i in range(n):
                            u, c = struct.unpack_from('<II', buf, cur + i * 8)
                            if u != c and comp == 10:
                                return True
                        cur += n * 8 + sum(struct.unpack_from('<II', buf, cur + i * 8)[1] + 4 for i in range(n))
                        pos = cur
                    else:
                        pos = p + 8
    except (OSError, ValueError):
        pass
    return False


def decode_blocks(data, decode, label='forge'):
    """Return decoded payload buffers from a scimitar entry.

    Raw entries without ``THFIRE_MAGIC`` are yielded as-is; entries containing
    ``THFIRE_MAGIC`` blocks yield each decoded block in order.
    """
    if data[:8] != THFIRE_MAGIC:
        return [data]
    out, pos = [], 0
    while True:
        p = data.find(THFIRE_MAGIC, pos)
        if p < 0:
            break
        if p + 19 > len(data):
            raise ValueError(f'{label}: truncated THFIRE header at {p:#x}')
        ver, comp, _, n = struct.unpack_from('<HBII', data, p + 8)
        if ver != 3 or not 1 <= n <= 100000:
            raise ValueError(f'{label}: unsupported THFIRE header (version {ver}, chunks {n}) at {p:#x}')
        cur = p + 19
        if cur + n * 8 > len(data):
            raise ValueError(f'{label}: truncated THFIRE chunk table at {p:#x}')
        chunks = [struct.unpack_from('<II', data, cur + i * 8) for i in range(n)]
        cur += n * 8
        buf = bytearray()
        for raw_len, packed_len in chunks:
            if cur + 4 + packed_len > len(data):
                raise ValueError(f'{label}: truncated THFIRE chunk at {cur:#x}')
            cur += 4   # 4-byte chunk checksum
            raw = data[cur:cur + packed_len]
            cur += packed_len
            if packed_len == raw_len:
                buf += raw
            elif comp == 10:
                if decode is None:
                    raise ValueError(f'{label}: Oodle decoder required for compressed THFIRE chunk')
                dec = decode(raw, raw_len)
                if len(dec) != raw_len:
                    raise ValueError(f'{label}: Oodle decoded {len(dec)} bytes, expected {raw_len}')
                buf += dec
            else:
                raise ValueError(f'{label}: unsupported THFIRE codec {comp}')
        out.append(bytes(buf))
        pos = cur
    return out


def read(f, entry, decode, label='forge'):
    """Seek to *entry* ``(offset, file_data_id, stored_size)`` and decode its blocks."""
    off, fid, stored = entry
    f.seek(off)
    data = f.read(stored)
    if len(data) != stored:
        raise ValueError(f'{label}#{fid:x}: truncated entry ({len(data)} of {stored} bytes)')
    return decode_blocks(data, decode, f'{label}#{fid:x}')
