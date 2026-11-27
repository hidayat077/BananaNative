"""Ubisoft's GPUCODEONLY shader archives (Anvil), without game data: each control section and the
code area that ends four bytes before it, headed for graphics and headerless for compute."""
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import shader_split


def blob(code_len, ps=True):
    """One code area, its 4-byte record and the control section that names its length"""
    ctl = bytearray(0x880)
    struct.pack_into('<I', ctl, 0, 0x98761234)
    struct.pack_into('<I', ctl, 0x20, 0x880)
    struct.pack_into('<I', ctl, 0x704, code_len)
    code = bytearray(code_len)
    if ps:
        struct.pack_into('<I', code, 0, 0x12345678)
        struct.pack_into('<I', code, 0x30, 5 << 10)   # SPH type 5: pixel
    else:
        struct.pack_into('<3I', ctl, 0x748, 8, 8, 1)   # headerless compute: thread group in the control
    return bytes(code) + struct.pack('<I', 0xdeadbeef) + bytes(ctl)


def archive():
    return (b'GPUCODEONLY\0' + struct.pack('<I', 0x300) + blob(0x300) +
            b'GPUCODEONLY\0' + struct.pack('<I', 0x300) + blob(0x200, ps=False))


class GpuCodeonlyTests(unittest.TestCase):
    def test_pairs_both_code_area_forms(self):
        out = []
        shader_split.gpu_codeonly_shaders(archive(), '/ShaderDataNVN.bin', out)
        self.assertEqual([(x[1], len(x[3])) for x in out], [('ps', 0x300), ('cs', 0x200)])
        self.assertTrue(all(x[0].startswith('/ShaderDataNVN.bin#GPU@0x') for x in out))
        self.assertEqual([x[3][:4] for x in out], [shader_split.GFX_MAGIC, b'\0\0\0\0'])

    def test_bad_lengths_are_left_out(self):
        d = bytearray(archive())
        struct.pack_into('<I', d, 0x314 + 0x20, 0x100)   # a control that claims an impossibly short section
        struct.pack_into('<I', d, 0x314 + 0x704, 0x2000)   # and a code length past the start of the file
        out = []
        shader_split.gpu_codeonly_shaders(bytes(d), '/ShaderDataNVN.bin', out)
        self.assertEqual([x[1] for x in out], ['cs'])

    def test_scan_file_dispatches_by_header(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'ShaderDataNVN.bin'
            path.write_bytes(archive())
            rel, keys, pairs, extra = shader_split.scan_file_in((str(path), 'ShaderDataNVN.bin'))
            self.assertEqual(len(keys), 2)
            self.assertEqual(extra, {})


if __name__ == '__main__':
    unittest.main()
