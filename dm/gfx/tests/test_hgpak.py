"""No Man's Sky's HGPAK archives and .nvb shader binaries, without game data: a synthetic pak, a synthetic program, and Oodle chunks."""
import os, struct, sys, tempfile, unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import shader_split

HGPAK = shader_split.HGPAK
CTL, GFX, CS = shader_split.CTL_MAGIC, bytes.fromhex('78563412'), bytes.fromhex('79563412')
CHUNK = shader_split.HGPAK_CHUNK


def nvb(stage='vs'):
    """One .nvb record: control +0x20 is its own length, +0x704 the code area's, and the SPH word at code +0x30 names the stage"""
    ctl = bytearray(0x880)
    struct.pack_into('<I', ctl, 0, int.from_bytes(CTL, 'little'))
    struct.pack_into('<I', ctl, 0x20, len(ctl))
    struct.pack_into('<I', ctl, 0x704, 0x40)
    code = bytearray(0x40)
    magic = CS if stage == 'cs' else GFX
    struct.pack_into('<I', code, 0, int.from_bytes(magic, 'little'))
    if stage != 'cs':
        bits = {'vs': 1, 'ps': 5, 'gs': 4}[stage]
        struct.pack_into('<I', code, 0x30, bits << 10 | 1)
    return bytes(ctl + code)


def pak(members, compressed=False, chunks=()):
    """A version-2 HGPAK: the manifest member first, then the named members; compressed data comes from chunks"""
    manifest = b''.join(name.encode() + b'\r\n' for name, _ in members)
    if compressed:
        data = b''.join(chunks)
        sizes = tuple(len(c) for c in chunks)
        dataoff = 0x30 + 0x20 * (len(members) + 1) + 8 * len(sizes)
        index = [manifest, *(body for _, body in members)]
    else:
        data = manifest + b''.join(body for _, body in members)
        sizes = ()
        dataoff = 0x30 + 0x20 * (len(members) + 1)
        index = None
    head = HGPAK + struct.pack('<QQQ?7xQ', 2, len(members) + 1, len(sizes), compressed, dataoff)
    rows = b''
    off = dataoff
    for size in ([len(manifest)] + [len(body) for _, body in members] if index is None else [len(manifest)] + [CHUNK] * len(members)):
        rows += struct.pack('<16sQQ', bytes(16), off, size)
        off += size if index is None else CHUNK
    return head + rows + (b''.join(struct.pack('<Q', s) for s in sizes)) + data


class HgpakTests(unittest.TestCase):
    def test_nvb_stage_and_key(self):
        out = shader_split.nvb_shaders(nvb('vs'), '/x/a.nvb')
        self.assertEqual(len(out), 1)
        where, st, ctl, code = out[0]
        self.assertEqual((where, st, len(ctl), len(code)), ('/x/a.nvb#NVB@0x0', 'vs', 0x880, 0x40))
        self.assertEqual(out[0][1], shader_split.glslc_stage(ctl, code))
        self.assertEqual(len(shader_split.nvb_shaders(nvb('cs'), '/x/c.nvb')), 1)
        self.assertEqual(shader_split.nvb_shaders(b'not a shader', '/x/no.nvb'), [])

    def test_uncompressed_pak(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, 'test.pak')
            with open(path, 'wb') as f: f.write(pak([('shaders/a.nvb', nvb())]))
            out = []
            shader_split.hgpak_shaders(path, 'NMSARC.Shaders.pak', out)
            self.assertEqual([(x[0], x[1]) for x in out], [('/NMSARC.Shaders.pak/shaders/a.nvb#NVB@0x0', 'vs')])

    def test_oodle_chunks_and_stored_whole(self):
        body = nvb('ps')
        manifest = b'shaders/b.nvb\r\n'
        first = manifest + bytes(CHUNK - len(manifest))
        second = body + bytes(CHUNK - len(body))
        # The second chunk's stored bytes are already CHUNK long, so the decoder must not be asked for them
        comp_first, comp_second = b'oodle first', second
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, 'test.pak')
            with open(path, 'wb') as f:
                f.write(HGPAK + struct.pack('<QQQ?7xQ', 2, 2, 2, True, 0x30 + 0x40 + 0x10)
                        + struct.pack('<16sQQ', bytes(16), 0x30 + 0x50, len(manifest))
                        + struct.pack('<16sQQ', bytes(16), 0x30 + 0x50 + CHUNK, len(body))
                        + struct.pack('<QQ', len(comp_first), len(comp_second)) + comp_first
                        + bytes((-len(comp_first)) % 16) + comp_second)
            calls = []
            real = shader_split.oodle
            shader_split.oodle = lambda comp, n: (calls.append((bytes(comp), n)), first)[1]
            try:
                out = []
                shader_split.hgpak_shaders(path, 'NMSARC.Shaders.pak', out)
            finally:
                shader_split.oodle = real
            self.assertEqual(calls, [(comp_first, CHUNK)])
            self.assertEqual([(x[0], x[1]) for x in out], [('/NMSARC.Shaders.pak/shaders/b.nvb#NVB@0x0', 'ps')])

    def test_jobs_take_hgpaks_only(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, 'test.pak')
            with open(path, 'wb') as f: f.write(pak([('shaders/a.nvb', nvb())]))
            jobs, taken = shader_split.hgpak_jobs([(path, 'NMSARC.Shaders.pak'), (os.path.join(d, 'x.bin'), 'x.bin')])
            self.assertEqual(jobs, [(path, 'NMSARC.Shaders.pak', (HGPAK,))])
            self.assertEqual(taken, {path})


if __name__ == '__main__':
    unittest.main()
