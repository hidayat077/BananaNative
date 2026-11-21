"""Final Fantasy Resonance's particle vertex shaders pick texture / sampler slots at run time from c2's tables (profile key tex_dyn_slots). The translator leaves Ryujinx's
fallback arrays of 32 buffer textures / samplers named <stage>_tb_cb0_40000000 and <stage>_s_cb0_40000000; vk_android_build.py classify must place them as the Vulkan ABI's
tex_dyn / samp_dyn kinds instead of refusing the shader with '?' (vka_shader.c refusal). The function is taken from the tool's source by name, as test_android_img_ubo.py does."""
import ast
from pathlib import Path
import re
import unittest

REPO = Path(__file__).resolve().parents[3]


def tool_function(name, ns):
    src = (REPO / 'dm/gfx/tools/vk_android_build.py').read_text(encoding='utf-8')
    node = next(n for n in ast.parse(src).body if isinstance(n, ast.FunctionDef) and n.name == name)
    exec(compile(ast.Module([node], []), 'vk_android_build.py', 'exec'), ns)
    return ns[name]


DYN, SAMP, COMBINED, SAMPLED, STORAGE_IMAGE, STORAGE_TEXEL, UBO, SSBO = 4, 0, 1, 2, 3, 5, 6, 7

NS = {'re': re, 'spv': type('spv', (), {'VK_UNIFORM_TEXEL': DYN, 'VK_SAMPLER': SAMP, 'VK_COMBINED': COMBINED,
                                        'VK_SAMPLED_IMAGE': SAMPLED, 'VK_STORAGE_IMAGE': STORAGE_IMAGE, 'VK_STORAGE_TEXEL': STORAGE_TEXEL,
                                        'VK_UNIFORM_BUFFER': UBO, 'VK_STORAGE_BUFFER': SSBO})(),
      'abi': type('abi', (), {k: k for k in ('KIND_CBUF', 'KIND_C1', 'KIND_DRVCB', 'KIND_SUPPORT', 'KIND_SBUF', 'KIND_TEX', 'KIND_TBUF', 'KIND_IMG',
                                             'KIND_SAMP', 'KIND_SAMP_TEX', 'KIND_SAMP_ID', 'KIND_TEX_UBO', 'KIND_SAMP_UBO', 'KIND_TEX_C2A', 'KIND_SAMP_C2A',
                                             'KIND_TEX_POOL', 'KIND_SAMP_POOL', 'KIND_IMG_UBO', 'KIND_UNKNOWN')})(),
      'NS': {'decode_ubo_tex': lambda n: None, 'decode_tex': lambda n: None, 'c2_array': lambda n, p: None, 'const_sampler': lambda n: 0},
      'POOL_SUFFIX': '_cb0_40000000', 'KIND_TEX_DYN': 'tex_dyn', 'KIND_SAMP_DYN': 'samp_dyn'}
NS['sampler_half'] = lambda n: ('samp', 0, 0)


def bind(name, desc, count):
    return {'name': name, 'desc': desc, 'count': count, 'dim': 5, 'id': 1, 'binding': 3}


class DynamicSlots(unittest.TestCase):
    classify = staticmethod(tool_function('classify', NS))

    def test_ffr_fallback_arrays_are_placed(self):
        tb = self.classify('vs', bind('vp_tb_cb0_40000000', DYN, 32), {}, None)
        self.assertEqual(tb['kind'], 'tex_dyn')
        self.assertEqual(tb['flags'], 0)   # no BF_NOSLOT: element k is slot k, not an undeclared slot
        samp = self.classify('vs', bind('vp_s_cb0_40000000', SAMP, 32), {}, None)
        self.assertEqual(samp['kind'], 'samp_dyn')

    def test_the_id_indexed_pools_are_unchanged(self):
        # a 33-entry array is the pool indexed by texture IDs (tex_pool / samp_pool), not FFR's slot-indexed fallback
        self.assertEqual(self.classify('vs', bind('vp_t_cb0_40000000', SAMPLED, 33), {}, None)['kind'], NS['abi'].KIND_TEX_POOL)
        self.assertEqual(self.classify('vs', bind('vp_s_cb0_40000000', SAMP, 33), {}, None)['kind'], NS['abi'].KIND_SAMP_POOL)

    def test_other_32_entry_names_are_not_dynamic(self):
        self.assertEqual(self.classify('vs', bind('vp_t_cb0_40000000', SAMPLED, 32), {}, None)['kind'], NS['abi'].KIND_UNKNOWN)   # decode_tex stub fails


if __name__ == '__main__':
    unittest.main()
