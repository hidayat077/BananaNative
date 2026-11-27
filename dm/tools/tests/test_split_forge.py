"""Ubisoft Anvil scimitar (.forge) index parsing, THFIRE decoding, and splitter integration."""
import io
import os
import struct
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'fuzz'))
import fuzz_shader_split as fz
ss = fz.ss
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'gfx' / 'tools'))
import forge


def _scimitar_archive(entries):
    """Build a synthetic scimitar .forge archive from [(fid, payload_bytes), ...]."""
    hdr_off = 0x41a
    table_off = 0x476
    data_off = table_off + len(entries) * 20
    body = bytearray()
    recs = bytearray()
    for fid, payload in entries:
        recs += struct.pack('<QQI', data_off + len(body), fid, len(payload))
        body += payload
    buf = bytearray(data_off)
    buf[0:9] = forge.SCIMITAR_MAGIC
    struct.pack_into('<I', buf, 9, 0x1c)
    struct.pack_into('<Q', buf, 0x0d, hdr_off)
    struct.pack_into('<I', buf, hdr_off, len(entries))
    struct.pack_into('<Q', buf, hdr_off + 0x34, table_off)
    buf[table_off:data_off] = recs
    return bytes(buf + body)


def _thfire_block(chunks, comp=10):
    """Pack [(raw_len, payload_bytes), ...] into one THFIRE chunked block."""
    out = bytearray(forge.THFIRE_MAGIC)
    out += struct.pack('<HBII', 3, comp, 0x80040000, len(chunks))
    for raw_len, payload in chunks:
        out += struct.pack('<II', raw_len, len(payload))
    for _, payload in chunks:
        out += struct.pack('<I', 0xdeadbeef) + payload
    return bytes(out)


class ForgeTest(unittest.TestCase):
    def test_entries_and_uncompressed_thfire_gpu_codeonly_to_splitter(self):
        ctl, code = fz.VS
        gpu_blob = b'GPUCODEONLY\x00' + code + struct.pack('<I', len(code)) + ctl
        meta = _thfire_block([(16, b'\x01\x00' + bytes(14))])
        payload = meta + b'\x00\x00\x00' + _thfire_block([(len(gpu_blob), gpu_blob)])
        raw_arc = _scimitar_archive([(0x18d3d355980, payload), (0x10, b'tiny')])
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / 'DataNX_ORP_TitleScreen_nvn.forge'
            p.write_bytes(raw_arc)
            self.assertFalse(forge.needs_oodle(str(p)))
            ents = forge.entries(str(p))
            self.assertEqual(len(ents), 1)   # 4-byte tiny entry below minimum THFIRE/shader size skipped
            jobs, taken = ss.forge_jobs([(str(p), p.name)])
            self.assertEqual(taken, {str(p)})
            self.assertEqual(len(jobs), 1)
            rel, keys, pairs, extra = ss.scan_file(jobs[0])
            self.assertEqual(rel, p.name)
            self.assertEqual(list(keys), [ss.skey(ctl, code)])
            self.assertEqual((pairs, extra), (set(), {}))

    def test_compressed_thfire_calls_decoder_and_validates_length(self):
        ctl, code = fz.VS
        gpu_blob = b'GPUCODEONLY\x00' + code + struct.pack('<I', len(code)) + ctl
        blk = _thfire_block([(len(gpu_blob), b'cmp1')], comp=10)
        raw_arc = _scimitar_archive([(0xabc, blk)])
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / 'DataNX_nvn.forge'
            p.write_bytes(raw_arc)
            self.assertTrue(forge.needs_oodle(str(p)))
            dec = mock.Mock(return_value=gpu_blob)
            with open(p, 'rb') as f:
                blocks = forge.read(f, forge.entries(str(p))[0], dec, p.name)
            dec.assert_called_once_with(b'cmp1', len(gpu_blob))
            self.assertEqual(blocks, [gpu_blob])

        bad = _thfire_block([(10, b'cmp1')], comp=10)
        with self.assertRaisesRegex(ValueError, 'Oodle decoded'):
            forge.decode_blocks(bad, lambda b, n: b'short', 'test')

    def test_source_label_splits_on_hash_for_targeted_scan(self):
        ctl, code = fz.VS
        gpu_blob = b'GPUCODEONLY\x00' + code + struct.pack('<I', len(code)) + ctl
        raw_arc = _scimitar_archive([(0x99, _thfire_block([(len(gpu_blob), gpu_blob)]))])
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / 'DataNX_nvn.forge'
            p.write_bytes(raw_arc)
            found = ss.Shaders()
            ss.forge_shaders(str(p), p.name, forge.entries(str(p)), found)
            where = next(iter(found.seen))[0]
            self.assertTrue(where.startswith(f'/{p.name}#99#GPU@'), where)


if __name__ == '__main__':
    unittest.main()
