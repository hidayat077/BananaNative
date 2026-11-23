"""Tests for vk_android_spv.safe_inversesqrt: NMax(x, smallest normal) before a scalar GLSL InverseSqrt."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools'))
import vk_android_spv as spv  # noqa: E402

SRC = '''
               OpCapability Shader
        %glsl = OpExtInstImport "GLSL.std.450"
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
      %float = OpTypeFloat 32
         %c0 = OpConstant %float 0
         %c1 = OpConstant %float 1
       %main = OpFunction %void None %fn
          %l = OpLabel
          %r = OpExtInst %float %glsl InverseSqrt %c0
          %o = OpFAdd %float %r %c1
               OpReturn
               OpFunctionEnd
'''


class SafeInverseSqrt(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        self.spv_as = shutil.which('spirv-as')
        self.spv_val = shutil.which('spirv-val')
        src = self.tmp / 'in.spvasm'
        src.write_text(SRC)
        out = self.tmp / 'in.spv'
        if self.spv_as:
            subprocess.run([self.spv_as, str(src), '-o', str(out)], check=True)
        self.data = out.read_bytes() if out.exists() else None

    def test_clamp(self):
        if self.data is None:
            self.skipTest('spirv-as not installed')
        m = spv.Module(self.data)
        self.assertEqual(m.data(), self.data)
        n = spv.safe_inversesqrt(m)
        self.assertEqual(n, 1)
        # the inserted NMax has opcode 12, instruction 80, and the tiny constant 0x00800000
        raw = m.data()
        import struct
        w = list(struct.unpack('<%dI' % (len(raw) // 4), raw))
        self.assertTrue(any(w[i] >> 16 == 7 and w[i] & 0xffff == 12 and w[i + 4] == 80 for i in range(5, len(w))))
        self.assertTrue(any(w[i] >> 16 == 4 and w[i] & 0xffff == 43 and w[i + 3] == 0x00800000 for i in range(5, len(w))))
        self.assertEqual(len([1 for i in range(5, len(w)) if w[i] >> 16 == 6 and w[i] & 0xffff == 12 and w[i + 4] == 32]), 1)
        if self.spv_val:
            out = self.tmp / 'out.spv'
            out.write_bytes(raw)
            subprocess.run([self.spv_val, '--target-env', 'vulkan1.3', str(out)], check=True)


if __name__ == '__main__':
    unittest.main()
