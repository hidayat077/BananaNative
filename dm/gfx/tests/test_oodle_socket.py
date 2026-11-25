"""The phone's Oodle pipe (shader_split.py's _SockPipe, DM_OODLE_ANDROID_SOCKET): a stream is sent
and answered over one Unix socket. The split writes a multi-megabyte stream, then waits for the
decoder's answer; an unbuffered socket can send only part of it, and the two sides deadlock (seen
on Z-A's arc/data.trpfs: the host sat in read(fd, ..., 1.2 MB)). No game data or decoder here: a
thread stands in for :oodle and echoes the stream back."""
import os
import socket
import struct
import sys
import threading
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
os.environ.setdefault('DM_GAME', '0100000000010000')   # shader_split reads the profile at import
import shader_split  # noqa: E402


class OodleSocketTests(unittest.TestCase):
    def round_trip(self, size, answer=None):
        name = 'test_oodle_socket_' + str(os.getpid()) + '_' + str(size)
        heard = {}

        def serve():
            srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            srv.bind('\0' + name)
            srv.listen(1)
            c, _ = srv.accept()
            f = c.makefile('rb')   # read exactly the header and the stream, as dm/host/hle_oodle.c does
            hdr = f.read(16)
            heard['hdr'] = hdr
            cl, rl = struct.unpack('<QQ', hdr)
            heard['comp'] = f.read(cl)
            c.sendall(struct.pack('<Q', rl if answer is None else answer))
            if answer is None: c.sendall(heard['comp'])
            c.close(); srv.close()

        t = threading.Thread(target=serve, daemon=True)
        t.start()
        old_sock = os.environ.get('DM_OODLE_ANDROID_SOCKET')
        old_log = os.environ.get('DM_OODLE_ANDROID_LOG')
        os.environ['DM_OODLE_ANDROID_SOCKET'] = name
        os.environ['DM_OODLE_ANDROID_LOG'] = ''
        shader_split._OODLE = None
        pipe = None
        try:
            comp = os.urandom(size)
            got, gone = shader_split.oodle_ask(comp, size)
            pipe = shader_split._OODLE[0]
        finally:
            if pipe is not None:
                pipe.stdin.close(); pipe.stdout.close(); pipe._s.close()
            shader_split._OODLE = None
            for k, v in (('DM_OODLE_ANDROID_SOCKET', old_sock), ('DM_OODLE_ANDROID_LOG', old_log)):
                if v is None: os.environ.pop(k, None)
                else: os.environ[k] = v
        t.join(5)
        self.assertFalse(t.is_alive(), 'the decoder never got the whole stream')
        self.assertEqual(heard['hdr'], struct.pack('<QQ', size, size))
        self.assertEqual(heard['comp'], comp)
        return got, gone

    def test_large_stream(self):
        got, gone = self.round_trip(3 << 20)   # bigger than one socket send
        self.assertFalse(gone)
        self.assertEqual(len(got), 3 << 20)

    def test_refused_stream(self):
        got, gone = self.round_trip(4096, answer=0)
        self.assertIsNone(got)
        self.assertFalse(gone)   # 0: the decoder refused the stream, the host is still there


if __name__ == '__main__':
    unittest.main()
