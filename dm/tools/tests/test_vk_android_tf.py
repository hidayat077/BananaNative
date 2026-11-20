"""vk_android_spv.tf_capture: a vertex shader gets the transform feedback capture, and the module stays valid for spirv-val"""
import os, shutil, subprocess, sys, tempfile, unittest
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'gfx', 'tools'))
import vk_android_spv as spv

VS = """
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Vertex %main "main" %pos %out0 %vid
OpDecorate %pos BuiltIn Position
OpDecorate %out0 Location 0
OpDecorate %vid BuiltIn VertexIndex
%void = OpTypeVoid
%fn = OpTypeFunction %void
%f32 = OpTypeFloat 32
%v4 = OpTypeVector %f32 4
%pv4 = OpTypePointer Output %v4
%i32 = OpTypeInt 32 1
%pi = OpTypePointer Input %i32
%pos = OpVariable %pv4 Output
%out0 = OpVariable %pv4 Output
%vid = OpVariable %pi Input
%one = OpConstant %f32 1
%c = OpConstantComposite %v4 %one %one %one %one
%main = OpFunction %void None %fn
%l = OpLabel
OpStore %pos %c
OpStore %out0 %c
OpReturn
OpFunctionEnd
"""


@unittest.skipUnless(shutil.which('spirv-as') and shutil.which('spirv-val'), 'spirv-tools missing')
class TfCapture(unittest.TestCase):
    def test_valid(self):
        with tempfile.TemporaryDirectory() as d:
            src, out = os.path.join(d, 'a.spvasm'), os.path.join(d, 'a.spv')
            open(src, 'w').write(VS)
            subprocess.run(['spirv-as', '--target-env', 'vulkan1.1', src, '-o', out], check=True)
            m = spv.Module(open(out, 'rb').read())
            tf = [{'stride': 24, 'outputs': [[0, 4], [5, 2]]}, [], [], []]   # location 5 is not written: (0, 1)
            self.assertTrue(spv.tf_capture(m, tf))
            open(out, 'wb').write(m.data())
            r = subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', out], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)

    def test_no_stream(self):
        with tempfile.TemporaryDirectory() as d:
            src, out = os.path.join(d, 'a.spvasm'), os.path.join(d, 'a.spv')
            open(src, 'w').write(VS)
            subprocess.run(['spirv-as', '--target-env', 'vulkan1.1', src, '-o', out], check=True)
            self.assertFalse(spv.tf_capture(spv.Module(open(out, 'rb').read()), [[], [], [], []]))


if __name__ == '__main__':
    unittest.main()
