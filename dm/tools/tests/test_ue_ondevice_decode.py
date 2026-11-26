"""The phone's Unreal shader decode uses the game's own Oodle (shader_split.oodle over OodleService), not the x86-only ooz worker. Synthetic worker replies only."""
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'fuzz'))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'gfx/tools'))
import ue_ondevice as uod
import uepak


class OnDeviceDecode(unittest.TestCase):
    def test_a_callable_decoder_is_used_without_an_ooz_worker(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            romfs, out = root / 'romfs', root / 'ue_shaders'
            romfs.mkdir()
            calls = []

            def decoder(comp, n):
                calls.append((comp, n))
                return b'\xaa' * n

            def job(r, main, work, decode):
                work = Path(work); work.mkdir(parents=True, exist_ok=True)
                comp = b'compressed'
                raw = 7
                with open(work / 'oodle.job', 'wb') as f:
                    f.write(struct.pack('<II', uepak.JOB_MAGIC, 1))
                    f.write(struct.pack('<II', len(comp), raw))
                    f.write(comp)
                self.assertEqual(decode(comp, raw), b'\xaa' * raw)
                return [{'name': 'archive', 'shaders': 1}], 1

            def finish(work, part):
                part = Path(part); part.mkdir(parents=True, exist_ok=True)
                (part / 'shaderarchive-global.ushaderbytecode').write_bytes(b'archive')

            with mock.patch.object(uod.uepak, 'io_shader_tocs', return_value=['Game.utoc']), \
                 mock.patch.object(uod.uepak, 'job', side_effect=job), \
                 mock.patch.object(uod.uepak, 'finish', side_effect=finish):
                got = uod.decode(str(romfs), 'main', str(out), decoder=decoder)

            self.assertEqual(got, str(out))
            self.assertEqual(calls, [(b'compressed', 7), (b'compressed', 7)])
            self.assertEqual(sorted(p.name for p in out.iterdir()),
                             ['shaderarchive-global.ushaderbytecode', 'stamp'])
            self.assertEqual((out / 'shaderarchive-global.ushaderbytecode').read_bytes(), b'archive')

    def test_no_decoder_at_all_is_a_clear_error(self):
        with tempfile.TemporaryDirectory() as d:
            romfs = Path(d) / 'romfs'; romfs.mkdir()
            with mock.patch.object(uod.uepak, 'io_shader_tocs', return_value=['Game.utoc']):
                with self.assertRaisesRegex(ValueError, 'Unreal shader archives need a decoder'):
                    uod.decode(str(romfs), 'main', str(Path(d) / 'ue_shaders'))


if __name__ == '__main__':
    unittest.main()
