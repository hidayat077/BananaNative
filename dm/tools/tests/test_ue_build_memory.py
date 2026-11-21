"""Build-time memory fixes found on Final Fantasy Resonance: modern Unreal paks and IoStore containers are not read whole by the shader split, and a decoded shader archive is written a record at a time."""
import importlib.util
import io
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'dm/gfx/tools'))
import uepak  # noqa: E402

PAK_MAGIC = 0x5A6F12E1   # uepak.py: the magic in a pak's footer


def pak(version, size):
    d = bytearray(size)
    struct.pack_into('<II', d, size - (0x2c if version == 4 else 0xc8), PAK_MAGIC, version)
    return bytes(d)


def archive(records):
    head = struct.pack('<5I', 2, 0, 0, 0, len(records))
    ents, off = bytearray(), 0
    for stored, raw in records:
        ents += struct.pack('<QIIB', off, len(stored), len(raw), 3)
        off += len(stored)
    return head + bytes(ents) + struct.pack('<II', 0, 0) + b''.join(st for st, _ in records)


class UnrealModern(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        os.environ.setdefault('DM_GAME', '0100606026F12000')
        spec = importlib.util.spec_from_file_location('shader_split_ue', ROOT / 'dm/gfx/tools/shader_split.py')
        cls.split = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.split)
        cls.tmp = tempfile.TemporaryDirectory()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_modern_pak_and_containers_are_not_read_whole(self):
        for name, data in (('FFRS-Switch.pak', pak(11, 1 << 20)), ('FFRS-Switch.ucas', b'\0' * (1 << 20)), ('FFRS-Switch.utoc', b'\0' * (1 << 20))):
            p = Path(self.tmp.name) / name
            p.write_bytes(data)
            self.assertTrue(self.split.ue_modern_pack(str(p)), name)
            _, uniq, pairs, extra = self.split.scan_file((str(p), name))
            self.assertEqual((uniq, pairs, extra), ({}, set(), {}), name)

    def test_legacy_pak_keeps_its_inline_shaders(self):
        p = Path(self.tmp.name) / 'pakchunk0-Switch.pak'
        p.write_bytes(pak(4, 1 << 20))
        self.assertFalse(self.split.ue_modern_pack(str(p)))


class ArchiveDecode(unittest.TestCase):
    def test_streamed_decode_matches_the_in_memory_form(self):
        raws = [b'LSLG-a', b'LSLG-b']
        packed = archive([(b'aa', raws[0]), (b'bbb', raws[1])])
        out = io.BytesIO()

        def records():
            yield raws[0]
            self.assertTrue(out.getvalue().endswith(raws[0]))   # each record is written before the next is produced
            yield raws[1]

        uepak.write_decoded_archive(out, packed, uepak.archive(packed), records())
        self.assertEqual(out.getvalue(), archive([(r, r) for r in raws]))
        self.assertEqual(uepak.decoded_archive(packed, raws), out.getvalue())


if __name__ == '__main__':
    unittest.main()
