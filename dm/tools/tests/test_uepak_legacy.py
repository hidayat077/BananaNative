"""Early Unreal paks: version 5's legacy footer and archive version 1's Zlib shaders; no game files or keys."""
import io
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import uepak


def v1_archive(raws):
    """A version 1 shader code archive: one 37-byte entry per shader, then its Zlib stream."""
    entries, code = bytearray(), bytearray()
    for fr, raw in raws:
        stored = zlib.compress(raw)
        entries += bytes(28) + struct.pack('<IIB', len(stored), len(raw), fr)
        code += stored
    return struct.pack('<II', 1, len(raws)) + bytes(entries) + bytes(code)


def v5_pak(index, version=5):
    """A legacy footer whose index names the files; the blob is the index itself."""
    return index + bytes((0,)) + struct.pack('<IIQQ', uepak.PAK_MAGIC, version, 0, len(index)) + bytes(20)


class Legacy(unittest.TestCase):
    def test_v1_archive_decodes_zlib_shaders(self):
        raws = [(5, b'LSLG' + bytes(100)), (3, b'LSLG' + bytes(60))]
        b = v1_archive(raws)
        a = uepak.archive(b)
        self.assertEqual(a['version'], 1)
        self.assertEqual([e[1:] for e in a['shaders']], [(len(zlib.compress(r)), len(r), fr) for fr, r in raws])
        self.assertEqual(a['code'], 8 + 37 * len(raws))
        self.assertEqual(list(uepak.zlib_records(b, a)), [r for _, r in raws])
        out = io.BytesIO()
        uepak.write_decoded_archive(out, b, a, uepak.zlib_records(b, a))
        d = out.getvalue()
        a2 = uepak.archive(d)
        self.assertEqual([e[1] for e in a2['shaders']], [len(r) for _, r in raws])
        self.assertEqual([e[2] for e in a2['shaders']], [len(r) for _, r in raws])
        self.assertEqual([d[a2['code'] + off:a2['code'] + off + size] for off, size, _, _ in a2['shaders']], [r for _, r in raws])

    def test_v5_footer_and_inline_detection(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for name, index, want in (('archive', b'../../../\0' + struct.pack('<i', 25) + b'.ushaderbytecode\0', False),
                                      ('inline', b'../../../\0' + struct.pack('<i', 5) + b'game.uexp\0', True)):
                p = root / f'{name}.pak'; p.write_bytes(v5_pak(index))
                with open(p, 'rb') as f:
                    enc, ioff, isz, _, ver = uepak.footer(f)
                self.assertEqual((enc, ioff, isz, ver), (0, 0, len(index), 5))
                self.assertEqual(uepak.inline_shaders(str(p)), want)


if __name__ == '__main__':
    unittest.main()
