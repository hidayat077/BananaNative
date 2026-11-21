#!/usr/bin/env python3
"""dm/third_party/ooz's unoodle, the PC builder's Oodle decoding worker: it builds from the pinned files, speaks the DM_OODLE_SERVE pipe protocol, and fails closed.
No game data: ooz has no compressor, so the valid streams here are the three forms that can be written by hand from the open decoder's reading of the format
(a block stored whole, a quantum stored whole, a quantum of one repeated byte). Entropy-coded Kraken and Mermaid streams are checked per game instead, by the
shader keys of the profile (dm/third_party/ooz/README.md "Checked"). Skipped where the worker cannot be built (Windows, not x86-64, no C++ compiler or patch)."""
import os, platform, shutil, struct, subprocess, sys, tempfile, unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
OOZ = REPO / 'dm' / 'third_party' / 'ooz'
BLOCK = 0x40000   # Oodle's block size: each block of a stream has its own 2-byte header


def cxx():
    return os.environ.get('CXX') or next((c for c in ('c++', 'g++', 'clang++') if shutil.which(c)), None)


def stored(body, kind=6):
    """A stream of blocks stored whole: 0xCC, the decoder type, the bytes"""
    return b''.join(bytes([0xCC, kind]) + body[i:i + BLOCK] for i in range(0, len(body), BLOCK))


def quantum(size_field, rest, kind=6):
    """One block of one quantum: 0x8C, the decoder type, the 3-byte quantum header (big endian), the rest"""
    return bytes([0x8C, kind]) + size_field.to_bytes(3, 'big') + rest


class Unoodle(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if sys.platform == 'win32' or platform.machine().lower() not in ('x86_64', 'amd64'): raise unittest.SkipTest('unoodle is built on x86-64 Linux and macOS only')
        if not cxx() or not shutil.which('patch') or not shutil.which('sh'): raise unittest.SkipTest('no C++ compiler or no patch program')
        cls.dir = tempfile.TemporaryDirectory()
        cls.exe = os.path.join(cls.dir.name, 'unoodle')
        r = subprocess.run(['sh', str(OOZ / 'build.sh'), cls.exe, cxx()], capture_output=True, text=True)
        if r.returncode: cls.dir.cleanup(); raise AssertionError(f'build.sh failed ({r.returncode}): {r.stdout}{r.stderr}')

    @classmethod
    def tearDownClass(cls):
        cls.dir.cleanup()

    def send(self, data):
        """The worker with data on its input, started as uepak.OodleHost starts it (two arguments, which it ignores) → (exit code, output, messages)"""
        p = subprocess.run([self.exe, 'host', '-'], input=data, capture_output=True, env=dict(os.environ, DM_OODLE_SERVE='1'))
        return p.returncode, p.stdout, p.stderr.decode(errors='replace')

    def ask(self, *reqs):
        """Each (compressed, raw length) in one run → the answers (bytes, or None for 'failed'); the worker must end with exit code 0 and nothing left over"""
        rc, out, err = self.send(b''.join(struct.pack('<QQ', len(c), n) + c for c, n in reqs))
        self.assertEqual(rc, 0, err)
        got, o = [], 0
        for _ in reqs:
            n = struct.unpack_from('<Q', out, o)[0]; o += 8
            got.append(out[o:o + n] if n else None); o += n
        self.assertEqual(o, len(out))
        return got

    def test_pinned_files(self):
        """The two upstream files are the pinned ones, byte for byte (build.sh checks the same hashes before it compiles)"""
        import hashlib
        text = (OOZ / 'build.sh').read_text()
        for f in ('kraken.cpp', 'stdafx.h'):
            self.assertIn(f'check {f} {hashlib.sha256((OOZ / f).read_bytes()).hexdigest()}', text)

    def test_known_answers(self):
        body = bytes(range(256)) * 5
        big = bytes((i * 7 + (i >> 9)) & 0xFF for i in range(BLOCK + 1000))
        reqs = [(stored(body, 6), len(body)), (stored(body, 10), len(body)), (stored(body, 12), len(body)),   # Kraken, Mermaid / Selkie, Leviathan block headers
                (stored(big), len(big)),                                                                    # two blocks
                (quantum(0x07FFFF, b'A'), 5000), (quantum(0x07FFFF, b'\0', 10), BLOCK),                     # a quantum of one repeated byte
                (quantum(len(body) - 1, body), len(body))]                                                  # a quantum stored whole
        want = [body, body, body, big, b'A' * 5000, b'\0' * BLOCK, body]
        self.assertEqual(self.ask(*reqs), want)

    def test_fails_closed(self):
        """A wrong raw length, bytes left over, a cut stream, garbage, an unknown decoder type and an empty request all answer 'failed'; the next stream still decodes"""
        body = bytes(range(256)) * 5
        good = (stored(body), len(body))
        bad = [(stored(body), len(body) + 1), (stored(body), len(body) - 1), (stored(body) + b'\0', len(body)), (stored(body)[:-1], len(body)),
               (quantum(0x07FFFF, b'A') + b'\0', 5000), (quantum(0x07FFFF, b''), 5000), (quantum(len(body) - 1, body[:-7]), len(body)),
               (bytes([0xCC, 7]) + body, len(body)), (bytes([0x9C, 6]) + body, len(body)), (bytes(range(1, 255)) * 9, 6000), (b'', 100), (b'x', 0), (b'', 0)]
        for k, req in enumerate(bad):
            with self.subTest(case=k): self.assertEqual(self.ask(req, good), [None, body])

    def test_other_decoders_refused(self):
        """LZNA (5) and Bitknit (11) are not built in: their quanta fail"""
        for kind in (5, 11):
            self.assertEqual(self.ask((bytes([0x8C, kind]) + bytes(40), 4096)), [None])

    def test_input_ends(self):
        """Input that ends inside a request, or a length over the limit, ends the worker with an error and no answer"""
        for data in (struct.pack('<QQ', 100, 100) + b'abc', struct.pack('<QQ', 5, (1 << 30) + 1) + b'abcde', struct.pack('<QQ', (1 << 30) + 1, 5)):
            rc, out, err = self.send(data)
            self.assertEqual((rc, out), (2, b'')); self.assertIn('unoodle: stream 1', err)
        self.assertEqual(self.send(b''), (0, b'', ''))

    def test_stats(self):
        with tempfile.TemporaryDirectory() as d:
            f = os.path.join(d, 'stats.txt')
            p = subprocess.run([self.exe], input=struct.pack('<QQ', 12, 10) + stored(b'0123456789'), capture_output=True, env=dict(os.environ, UNOODLE_STATS=f))
            self.assertEqual(p.returncode, 0)
            self.assertIn('streams=1 | type=6(kraken) ok=1 failed=0 in=12 out=10', Path(f).read_text())


if __name__ == '__main__':
    unittest.main()
