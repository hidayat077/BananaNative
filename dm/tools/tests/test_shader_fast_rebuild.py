"""Unit tests for scripts/shader_fast_rebuild.py: library scanning, key filtering, and .r migration."""
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'scripts'))
sys.path.insert(0, str(ROOT / 'dm/gfx/tools'))

import shader_fast_rebuild as sfr  # noqa: E402
import vk_android_delta as delta  # noqa: E402


def _make_fake_lib(path: Path, version: str, inputs: str, entries_spec: list[tuple[int, int, list[tuple[int, int, int, int]]]]):
    """Create a minimal valid dmsh_vk.lib with given (key, stage, [(kind, desc, flags, dim_byte)]) entries."""
    hs = delta.VK['HEADER_SIZE']
    entries = bytearray()
    binds = bytearray()
    mods = bytearray()
    blob = bytearray()
    nb = nm = 0

    for key, st, blist in entries_spec:
        c1 = b'\x01\x02\x03\x04'
        c1_off = len(blob)
        blob += c1 + b'\0' * 12

        spv = b'\x03\x02\x23\x07' + struct.pack('<IIII', 0x10300, 0, 8, 0)
        m0 = nm
        mod_off = len(blob)
        blob += spv + b'\0' * ((16 - len(spv) % 16) % 16)
        mods += delta.MOD.pack(mod_off, len(spv), len(spv), 0, delta.VK['CODEC_RAW'], 0)
        nm += 1

        b0 = nb
        for kind, desc, bflags, dim_b in blist:
            binds += sfr.BIND_STRUCT.pack(kind, desc, 0, 0, bflags, 1, -1, 0, 0, dim_b, -1, 0, 0, 0)
            nb += 1

        entries += delta.ENTRY.pack(key, 0, st, 1, 0, m0, b0, len(blist), 0, 1, 1, 1, 0, 0, 0, c1_off, len(c1), 0)

    eo = hs
    bo = eo + len(entries)
    mo = bo + len(binds)
    blob_off = (mo + len(mods) + 15) // 16 * 16
    pad = lambda s: s.encode()[:16].ljust(16, b'\0')
    head = delta.HEAD.pack(
        delta.VK['LIB_MAGIC'],
        delta.VK['LIB_VERSION'],
        pad('4b53c1440ada'),
        pad(version),
        pad('0100606026F12000'),
        pad(inputs),
        len(entries_spec),
        eo,
        nb,
        bo,
        nm,
        mo,
        blob_off,
        len(blob),
        64,
        64,
        delta.abi_digest().encode(),
    )
    data = bytearray(head + entries + binds + mods)
    data += b'\0' * (blob_off - len(data))
    data += blob
    path.write_bytes(data)


class ShaderFastRebuildTest(unittest.TestCase):
    def test_scan_filter_and_migrate_from_lib(self):
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            lib = d / 'dmsh_vk.lib'
            _make_fake_lib(
                lib,
                'f2b04744b54a',
                '2ddf71af58a7',
                [
                    (0x1111111111111111, delta.VK['STAGE_FS'], [(8, 1, 0, 1)]),    # plain ps
                    (0x2222222222222222, delta.VK['STAGE_VS'], [(8, 1, 0, 5)]),    # buffer+combined vs
                    (0x3333333333333333, delta.VK['STAGE_CS'], [(10, 3, 1, 1)]),   # cs + rw
                ],
            )
            scan = sfr.scan_library(lib)
            self.assertEqual(scan.total, 3)
            self.assertEqual(scan.counts['ps'], 1)
            self.assertEqual(scan.counts['vs'], 1)
            self.assertEqual(scan.counts['cs'], 1)
            self.assertEqual(scan.counts['buf_combined'], 1)
            self.assertEqual(scan.counts['bf_rw'], 1)

            reb, keep = sfr.select_rebuild_keys(scan, sfr.F_CS | sfr.F_BUF_COMBINED)
            self.assertEqual(reb, {'2222222222222222', '3333333333333333'})
            self.assertEqual(keep, {'1111111111111111'})

            res_dir = d / 'results'
            st = sfr.migrate_to_results(lib, res_dir, '520bbb51fb3c', '2ddf71af58a7', 'val1', keep)
            self.assertEqual((st['from_lib'], st['from_r'], st['already']), (1, 0, 0))

            rfile = res_dir / '1111111111111111.r'
            self.assertTrue(sfr._is_valid_target_r(rfile, '520bbb51fb3c', '2ddf71af58a7', 'val1'))
            with open(rfile, 'rb') as f:
                assert f.read(8) == sfr.RESULT_MAGIC
                n = struct.unpack('<I', f.read(4))[0]
                hdr = json.loads(f.read(n))
            self.assertEqual(hdr['vk'], '520bbb51fb3c')
            self.assertEqual(hdr['reused'], 'f2b04744b54a')
            self.assertIn('lib', hdr['refl'])

    def test_classify_changed_files(self):
        m, full, _ = sfr.classify_changed_files([
            'dm/third_party/ryujinx/src/Ryujinx.Graphics.Shader/StructuredIr/AstOptimizer.cs',
            'dm/gfx/tools/vk_android_spv.py',
        ], 'buffer_textures_split')
        self.assertFalse(full)
        self.assertEqual(m, sfr.F_CS | sfr.F_BF_RW | sfr.F_BUF_COMBINED | sfr.F_TBUF)

        _, full2, _ = sfr.classify_changed_files(['dm/third_party/ryujinx/src/Ryujinx.Graphics.Shader/CodeGen/Spirv/SpirvGenerator.cs'])
        self.assertTrue(full2)

    def test_ps_to_vs_pair_expansion(self):
        with tempfile.TemporaryDirectory() as td:
            lib = Path(td) / 'dmsh_vk.lib'
            _make_fake_lib(
                lib, 'f2b04744b54a', '2ddf71af58a7',
                [
                    (0x1111111111111111, delta.VK['STAGE_FS'], [(8, 1, 0, 1)]),
                    (0x2222222222222222, delta.VK['STAGE_VS'], [(8, 1, 0, 1)]),
                    (0x3333333333333333, delta.VK['STAGE_VS'], [(8, 1, 0, 1)]),
                ],
            )
            scan = sfr.scan_library(lib)
            pairs = {'1111111111111111': {'2222222222222222'}}
            reb, keep = sfr.select_rebuild_keys(scan, 0, {'1111111111111111'}, ps_to_vs=pairs)
            self.assertEqual(reb, {'1111111111111111', '2222222222222222'})
            self.assertEqual(keep, {'3333333333333333'})

    def test_migrate_ignores_stale_other_version_results_dir(self):
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            lib = d / 'v-f2b04744b54a' / 'dmsh_vk.lib'
            lib.parent.mkdir(parents=True)
            _make_fake_lib(lib, 'f2b04744b54a', '2ddf71af58a7', [(0x1111111111111111, delta.VK['STAGE_VS'], [(8, 1, 0, 1)])])

            older_res = d / 'v-000000000000' / 'results'
            older_res.mkdir(parents=True)
            stale_hdr = json.dumps({
                'vk': '000000000000', 'inputs': '2ddf71af58a7', 'raw': False, 'val': 'val1',
                'key': '1111111111111111', 'stage': 'vs', 'st': 'ok', 'err': '', 'modules': [[0, 4, 4, 0]],
            }, separators=(',', ':')).encode()
            (older_res / '1111111111111111.r').write_bytes(sfr.RESULT_MAGIC + struct.pack('<I', len(stale_hdr)) + stale_hdr + b'OLD!')

            res_dir = d / 'v-520bbb51fb3c' / 'results'
            res_dir.mkdir(parents=True)
            prev_bad_hdr = json.dumps({
                'vk': '520bbb51fb3c', 'inputs': '2ddf71af58a7', 'raw': False, 'val': 'val1',
                'key': '1111111111111111', 'stage': 'vs', 'st': 'ok', 'err': '', 'modules': [[0, 4, 4, 0]],
            }, separators=(',', ':')).encode()
            (res_dir / '1111111111111111.r').write_bytes(sfr.RESULT_MAGIC + struct.pack('<I', len(prev_bad_hdr)) + prev_bad_hdr + b'BAD!')

            st = sfr.migrate_to_results(lib, res_dir, '520bbb51fb3c', '2ddf71af58a7', 'val1', {'1111111111111111'}, results_dirs=[older_res])
            self.assertEqual((st['from_lib'], st['from_r'], st['already']), (1, 0, 0))
            with open(res_dir / '1111111111111111.r', 'rb') as f:
                assert f.read(8) == sfr.RESULT_MAGIC
                n = struct.unpack('<I', f.read(4))[0]
                hdr = json.loads(f.read(n))
            self.assertEqual(hdr['reused'], 'f2b04744b54a')


if __name__ == '__main__':
    unittest.main()
