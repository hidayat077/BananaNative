"""When the phone's Prepare starts the game's own Oodle decoder (OodleNeed.java with
android/app/src/jvmtest's OodleNeedJvmTest): only for a pack that holds the rewritten main under the name
the rewriter actually writes, and a profile that names a container the split decodes. The old check looked
for main.android.bin, which nothing writes, so on-device Prepare never started the decoder and the split
stopped with "Oodle- or LZ5-compressed game files: ..." (docs/reference/shaders.md). The pack's own game.tsv
is written after the split, so the APK's profile is the fallback. Skipped without a JDK."""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
JAVA = REPO / 'android/app/src/main/java/dev/nxnative/android/OodleNeed.java'
TEST = REPO / 'android/app/src/jvmtest/java/dev/nxnative/android/OodleNeedJvmTest.java'

TRINITY = 'romfs_marker\tarc/data.trpfs\n'


@unittest.skipUnless(shutil.which('javac') and shutil.which('java'), 'no JDK')
class OodleNeedTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix='oodle_need_'))
        cls.classes = cls.tmp / 'classes'
        subprocess.run(['javac', '-Xlint:all', '-Werror', '-d', str(cls.classes), str(JAVA), str(TEST)],
                       check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def needed(self, pack_files, profile_files):
        pack, prof = self.tmp / 'pack', self.tmp / 'profile'
        shutil.rmtree(pack, ignore_errors=True)
        shutil.rmtree(prof, ignore_errors=True)
        pack.mkdir()
        prof.mkdir()
        for base, files in ((pack, pack_files), (prof, profile_files)):
            for name, body in files.items():
                p = base / name
                if body is None: p.mkdir(parents=True)   # an unreadable "game.tsv" (a directory)
                else: p.write_text(body)
        r = subprocess.run(['java', '-cp', str(self.classes), 'dev.nxnative.android.OodleNeedJvmTest',
                            str(pack), str(prof)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout.strip() == 'true'

    def test_rewritten_main_and_profile_container(self):
        self.assertTrue(self.needed({'main.patched.bin': ''}, {'game.tsv': TRINITY}))
        self.assertTrue(self.needed({'main.patched.bin': '', 'game.tsv': TRINITY}, {}))

    def test_no_rewritten_main(self):
        self.assertFalse(self.needed({'game.tsv': TRINITY}, {}))
        # the file the old check asked for: writing it changes nothing
        self.assertFalse(self.needed({'main.android.bin': '', 'game.tsv': TRINITY}, {}))

    def test_profile_fallback(self):
        self.assertTrue(self.needed({'main.patched.bin': ''}, {'game.tsv': TRINITY}))
        self.assertTrue(self.needed({'main.patched.bin': '', 'game.tsv': 'name\tTest\n'},
                                    {'game.tsv': TRINITY}))
        self.assertTrue(self.needed({'main.patched.bin': '', 'game.tsv': None}, {'game.tsv': TRINITY}))
        self.assertFalse(self.needed({'main.patched.bin': ''}, {'game.tsv': 'name\tTest\n'}))

    def test_containers(self):
        for body in ('romfs_marker\tarc/data.trpfs\n', '# Oodle PKZ\n', '# gfpak archives\n',
                     '# Unreal .pak files\n', 'ROMFS_MARKER\tARC/DATA.TRPFS\n'):
            with self.subTest(body=body):
                self.assertTrue(self.needed({'main.patched.bin': ''}, {'game.tsv': body}))
        self.assertFalse(self.needed({'main.patched.bin': ''}, {'game.tsv': 'name\tTest\nromfs_marker\tdata.bin\n'}))


if __name__ == '__main__':
    unittest.main()
