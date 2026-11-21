#!/usr/bin/env python3
"""Fast incremental shader rebuild tool and first-time builder helper for Android packs.

Supports two workflows for any game profile in dm/games/<title_id>/:
1. Fast Hack Rebuild: inspect an older dmsh_vk.lib (and optional results/ directory),
   keep unaffected shaders by migrating valid .r files or extracting raw entries directly
   from dmsh_vk.lib into v-<DMSH_VK_CHAIN>/results/, and run vk_android_win.py only for
   the selected/affected shader subset.
2. First-Time / Full Build: run pack_android.py (split + code + shaders) or
   vk_android_win.py on the full shader set from scratch.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / 'scripts'))
sys.path.insert(0, str(REPO / 'dm/games'))
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))

import bananative_env  # noqa: E402
import dmsh_chain  # noqa: E402
import game as game_mod  # noqa: E402
import pack_dump  # noqa: E402
import vk_android_delta as delta  # noqa: E402

VK = delta.VK
STAGE_NAME = {
    VK['STAGE_VS']: 'vs',
    VK['STAGE_TCS']: 'hs',
    VK['STAGE_TES']: 'ds',
    VK['STAGE_GS']: 'gs',
    VK['STAGE_FS']: 'ps',
    VK['STAGE_CS']: 'cs',
    VK['STAGE_TS']: 'ts',
    VK['STAGE_MS']: 'ms',
}
PACK_ANDROID = REPO / 'scripts' / 'pack_android.py'
VK_BUILD_WIN = REPO / 'dm' / 'gfx' / 'tools' / 'vk_android_win.py'
HEADER_VK = REPO / 'dm' / 'gfx' / 'nvn' / 'dmsh_chain_vk.h'
RESULT_MAGIC = b'DMVKRES1'
WINDOWS = os.name == 'nt'

# Per-shader bitmask flags produced by scan_library()
F_VS = 1 << 0
F_PS = 1 << 1
F_CS = 1 << 2
F_GS = 1 << 3
F_TS = 1 << 4
F_MS = 1 << 5
F_HS = 1 << 6
F_DS = 1 << 7
F_BUF_COMBINED = 1 << 8    # Dim == Buffer (5) and desc == COMBINED_IMAGE_SAMPLER (1)
F_BF_RW = 1 << 9           # Any binding with DMSH_VK_BF_RW (1)
F_TBUF = 1 << 10           # Uniform or storage texel buffer (desc 4 or 5)
F_BINDLESS = 1 << 11       # tex_dyn / samp_dyn / tex_pool / samp_pool
F_GMEM_SBUF = 1 << 12      # Storage buffer or GMEM map binding

STAGE_FLAG_MAP = {
    'vs': F_VS,
    'hs': F_HS,
    'ds': F_DS,
    'gs': F_GS,
    'ps': F_PS,
    'cs': F_CS,
    'ts': F_TS,
    'ms': F_MS,
}

BIND_STRUCT = struct.Struct('<BBHBBHhHBBhHHI')
KEY_RE = re.compile(r'\b[0-9a-fA-F]{16}\b')

PRESETS: dict[str, tuple[str, int]] = {
    'auto': ('Auto-detect from recent Git changes', 0),
    'compute': ('Compute / Particle / Sort fixes', F_CS | F_BF_RW),
    'buffer_bindless': ('Texture / Buffer / Bindless fixes', F_BUF_COMBINED | F_TBUF | F_BINDLESS),
    'geom_mesh': ('Geometry / Mesh / Vertex fixes', F_VS | F_GS | F_HS | F_DS | F_TS | F_MS),
    'keys_only': ('Specific shaders from log only', 0),
    'custom': ('Custom (manual checkboxes)', 0),
}


def classify_changed_files(files: list[str], diff_text: str = '') -> tuple[int, bool, str]:
    """Map changed repo file paths to (filter_mask, recommend_full, human_summary)."""
    if not files:
        return 0, False, 'No changed files detected in git.'

    full_triggers = (
        'SpirvGenerator.cs',
        'Translator.cs',
        'TranslatorContext.cs',
        'Declarations.cs',
        'dmsh_abi.h',
        'dmsh_abi_vk.h',
        'Program.cs',
    )
    hit_full = [f for f in files if any(f.endswith(t) for t in full_triggers)]
    if hit_full:
        names = ', '.join(Path(f).name for f in hit_full[:3])
        return 0, True, f'Core translator/ABI changed ({names}) — Full Build recommended.'

    mask = 0
    reasons: list[str] = []
    for f in files:
        bn = Path(f).name
        if bn in ('AstOptimizer.cs', 'InstEmitSurface.cs', 'GeometryToCompute.cs'):
            mask |= F_CS | F_BF_RW
            reasons.append(f'{bn} -> Compute + BF_RW')
        elif bn in ('BindlessElimination.cs', 'BindlessToArray.cs'):
            mask |= F_BINDLESS
            reasons.append(f'{bn} -> Bindless')
        elif bn in ('InstEmitAmpereMesh.cs', 'MeshShaderInfo.cs', 'SpirvMeshEntry.cs'):
            mask |= F_TS | F_MS
            reasons.append(f'{bn} -> Task/Mesh')
        elif bn == 'GlobalToStorage.cs':
            mask |= F_GMEM_SBUF
            reasons.append(f'{bn} -> Storage/GMEM')
        elif bn in ('vk_android_spv.py', 'vk_android_build.py'):
            matched = False
            if 'tf_capture' in diff_text or 'MOD_TF' in diff_text or 'DMSH_VK_F_TF' in diff_text:
                mask |= F_VS
                reasons.append(f'{bn} -> Vertex (Transform Feedback)')
                matched = True
            if 'buffer_textures_split' in diff_text or 'DIM_BUFFER' in diff_text:
                mask |= F_BUF_COMBINED | F_TBUF
                reasons.append(f'{bn} -> Buffer+Combined / TexelBuffer')
                matched = True
            if not matched:
                mask |= F_CS | F_BUF_COMBINED | F_BF_RW
                reasons.append(f'{bn} -> Compute + Buffer+Combined + BF_RW')

    if mask:
        return mask, False, 'Detected: ' + '; '.join(dict.fromkeys(reasons))

    chain_touched = any(f.startswith(('dm/gfx/tools/', 'dm/third_party/ryujinx/', 'dm/gfx/nvn/')) for f in files)
    if not chain_touched:
        return 0, False, 'Only host/UI/docs changed (no shader chain files) — shader rebuild likely unnecessary.'
    return 0, True, 'Unknown shader chain change — Full Build recommended.'


def detect_git_filter(since_vk_version: str | None = None) -> tuple[int, bool, str]:
    """Inspect changes since the commit of since_vk_version (or latest commits/working tree) and suggest a filter mask."""
    try:
        base_ref = None
        if since_vk_version:
            lg = subprocess.run(
                ['git', '-C', str(REPO), 'log', '-S', since_vk_version, '-n', '1', '--format=%H', '--', 'dm/gfx/nvn/dmsh_chain_vk.h'],
                capture_output=True, text=True, timeout=5,
            )
            cand = lg.stdout.strip().splitlines()
            if cand:
                base_ref = cand[0]

        if base_ref:
            r = subprocess.run(['git', '-C', str(REPO), 'diff', '--name-only', base_ref], capture_output=True, text=True, timeout=5)
            files = [l.strip() for l in r.stdout.splitlines() if l.strip()]
            d = subprocess.run(['git', '-C', str(REPO), 'diff', base_ref, '--', 'dm/gfx/tools/'], capture_output=True, text=True, timeout=5)
            diff_text = d.stdout
            mask, rec_full, note = classify_changed_files(list(dict.fromkeys(files)), diff_text)
            if files:
                return mask, rec_full, f'(Since v-{since_vk_version}) {note}'

        r = subprocess.run(['git', '-C', str(REPO), 'diff', '--name-only', 'HEAD'], capture_output=True, text=True, timeout=5)
        files = [l.strip() for l in r.stdout.splitlines() if l.strip()]
        if files:
            d = subprocess.run(['git', '-C', str(REPO), 'diff', 'HEAD', '--', 'dm/gfx/tools/'], capture_output=True, text=True, timeout=5)
            diff_text = d.stdout
        else:
            r2 = subprocess.run(['git', '-C', str(REPO), 'log', '-n', '2', '--name-only', '--pretty=format:'], capture_output=True, text=True, timeout=5)
            files = [l.strip() for l in r2.stdout.splitlines() if l.strip()]
            d2 = subprocess.run(['git', '-C', str(REPO), 'diff', 'HEAD~2..HEAD', '--', 'dm/gfx/tools/'], capture_output=True, text=True, timeout=5)
            diff_text = d2.stdout
        return classify_changed_files(list(dict.fromkeys(files)), diff_text)
    except (OSError, subprocess.SubprocessError) as e:
        return 0, False, f'Git detection unavailable ({e}).'


def load_ps_to_vs_pairs(title_id: str) -> dict[str, set[str]]:
    """Load {ps_key: {vs_key, ...}} from the game profile's shader_pairs.tsv and pso_pairs.tsv."""
    ps_to_vs: dict[str, set[str]] = {}
    gdir = REPO / 'dm' / 'games' / title_id
    for fname in ('shader_pairs.tsv', 'pso_pairs.tsv'):
        p = gdir / fname
        if not p.is_file():
            continue
        for line in p.read_text(encoding='utf-8', errors='replace').splitlines():
            if not line or line.startswith('#') or '\t' not in line:
                continue
            parts = line.split('\t')
            if len(parts) >= 2:
                vs, ps = parts[0].strip().lower(), parts[1].strip().lower()
                if len(vs) == 16 and len(ps) == 16:
                    ps_to_vs.setdefault(ps, set()).add(vs)
    return ps_to_vs


@dataclass
class LibraryScan:
    path: Path
    version: str
    chain: str
    game_id: str
    inputs: str
    total: int
    key_flags: dict[str, int]
    counts: dict[str, int]


@dataclass
class GameContext:
    input_path: Path
    game_source: Path | None     # .nup or dump directory if known
    work_dir: Path
    pack_dir: Path
    pkg_dir: Path
    lib_dir: Path
    title_id: str
    game_name: str
    target_vk: str
    target_inputs: str
    val_id: str
    target_lib_exists: bool
    target_lib_current: bool
    pkg_exists: bool
    old_libs: list[Path] = field(default_factory=list)
    old_results_dirs: list[Path] = field(default_factory=list)
    profile_error: str | None = None


def current_vk_version() -> str:
    m = re.search(r'#define\s+DMSH_VK_CHAIN\s+"([0-9a-f]+)"', HEADER_VK.read_text(encoding='utf-8'))
    if not m:
        raise RuntimeError(f'DMSH_VK_CHAIN not found in {HEADER_VK}')
    return m.group(1)


def spirv_val_version(spirv_val: str | None = None) -> str:
    sv = spirv_val or os.environ.get('SPIRV_VAL') or shutil.which('spirv-val')
    if not sv:
        return 'missing'
    r = subprocess.run([sv, '--version'], capture_output=True, text=True)
    lines = r.stdout.strip().splitlines()
    return lines[0] if lines else 'unknown'


def _read_tsv_kv(path: Path) -> dict[str, str]:
    out: dict[str, str] = {}
    if not path.is_file():
        return out
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        if '\t' in line and not line.startswith('#'):
            k, v = line.split('\t', 1)
            out[k] = v
    return out


def _detect_from_work(work: Path) -> tuple[str, Path | None]:
    pack_tsv = work / 'android-pack' / 'pack.tsv'
    kv = _read_tsv_kv(pack_tsv)
    tid = kv.get('title_id', '').upper()
    game_src: Path | None = None
    if 'romfs' in kv:
        romfs_ref = kv['romfs'].split(':', 1)[0]
        cand = work.parent / romfs_ref
        if cand.is_file():
            game_src = cand
        else:
            # Dump directory: strip /base/romfs or /updates/.../romfs
            parts = Path(kv['romfs']).parts
            if parts:
                cdir = work.parent / parts[0]
                if cdir.is_dir():
                    game_src = cdir
    if not tid:
        game_tsv = _read_tsv_kv(work / 'android-pack' / 'game.tsv')
        tid = game_tsv.get('title_id', '').upper()
    if not tid and work.name.lower().endswith('-work') and len(work.name) == 21:
        tid = work.name[:16].upper()
    if not tid or not re.fullmatch(r'[0-9A-F]{16}', tid):
        raise ValueError(f'Could not detect Title ID from work folder: {work}')
    return tid, game_src


def inspect_source(path_str: str | Path) -> GameContext:
    """Inspect a .nup file, dump folder, or <id>-work directory."""
    bananative_env.apply()
    p = Path(path_str).expanduser().resolve()
    if not p.exists():
        raise FileNotFoundError(f'Path not found: {p}')

    game_src: Path | None = None
    if (p / 'android-shaders').is_dir() or (p / 'android-pack').is_dir() or p.name.lower().endswith('-work'):
        work = p
        tid, game_src = _detect_from_work(work)
        pack_dir = work / 'android-pack'
    else:
        app, _, _ = pack_dump.scan(p)
        tid = f'{app:016X}'
        game_src = p
        work = p.parent / f'{tid}-work'
        pack_dir = work / 'android-pack'

    profile_err = None
    gname = '?'
    target_inputs = ''
    try:
        g = game_mod.load(tid)
        gname = g.get('name', '?')
        target_inputs = dmsh_chain.inputs(g)
    except SystemExit:
        profile_err = f'Profile dm/games/{tid} not found'

    target_vk = current_vk_version()
    lib_dir = work / 'android-shaders' / 'lib'
    pkg_dir = work / 'android-shaders' / 'pkg'
    target_dir = lib_dir / f'v-{target_vk}'
    target_lib = target_dir / VK['LIB_FILE']
    stamp = _read_tsv_kv(target_dir / dmsh_chain.STAMP)
    target_current = (
        target_lib.is_file()
        and stamp.get('game') == tid
        and bool(target_inputs)
        and stamp.get('inputs') == target_inputs
    )

    old_libs: list[Path] = []
    seen_real: set[Path] = set()
    for root in (lib_dir, pack_dir / 'shaders'):
        if not root.is_dir():
            continue
        for sub in sorted(root.iterdir(), key=lambda x: -x.stat().st_mtime if x.exists() else 0):
            cand = sub / VK['LIB_FILE']
            if cand.is_file():
                rp = cand.resolve()
                if rp not in seen_real:
                    seen_real.add(rp)
                    old_libs.append(cand)

    old_results: list[Path] = []
    seen_res: set[Path] = set()
    ash = work / 'android-shaders'
    if ash.is_dir():
        cands = [r for r in list(ash.glob('lib/v-*/results')) + list(ash.glob('*/results')) if r.is_dir()]
        cands.sort(key=lambda x: -x.stat().st_mtime)
        for res in cands:
            rp = res.resolve()
            if rp not in seen_res:
                seen_res.add(rp)
                old_results.append(res)

    return GameContext(
        input_path=p,
        game_source=game_src,
        work_dir=work,
        pack_dir=pack_dir,
        pkg_dir=pkg_dir,
        lib_dir=lib_dir,
        title_id=tid,
        game_name=gname,
        target_vk=target_vk,
        target_inputs=target_inputs,
        val_id=spirv_val_version(),
        target_lib_exists=target_lib.is_file(),
        target_lib_current=target_current,
        pkg_exists=pkg_dir.is_dir() and any(pkg_dir.glob('*.bin')),
        old_libs=old_libs,
        old_results_dirs=old_results,
        profile_error=profile_err,
    )


def scan_library(lib_path: str | Path) -> LibraryScan:
    """Fast scan of a dmsh_vk.lib header, entries, and bindings into per-key feature bitmasks."""
    lp = Path(lib_path)
    kind_tbuf = VK['KIND_TBUF']
    kind_dyn = (VK['KIND_TEX_DYN'], VK['KIND_SAMP_DYN'], VK['KIND_TEX_POOL'], VK['KIND_SAMP_POOL'])
    kind_gmem = (VK['KIND_SBUF'], VK['KIND_GMEM_MAP'])

    L = delta.Library(str(lp))
    try:
        key_flags: dict[str, int] = {}
        counts = {
            'vs': 0, 'hs': 0, 'ds': 0, 'gs': 0, 'ps': 0, 'cs': 0, 'ts': 0, 'ms': 0,
            'buf_combined': 0, 'bf_rw': 0, 'tbuf': 0, 'bindless': 0, 'gmem_sbuf': 0,
        }
        m = L.m
        eo, bo = L.eo, L.bo
        esize, bsize = delta.ENTRY.size, VK['BIND_SIZE']
        for i in range(L.ne):
            key, _, st, _, _, _, b0, bc = struct.unpack_from('<QQBBHIIH', m, eo + i * esize)
            ks = f'{key:016x}'
            sname = STAGE_NAME.get(st, '')
            flags = STAGE_FLAG_MAP.get(sname, 0)
            if sname in counts:
                counts[sname] += 1
            for j in range(bc):
                kind, desc, _, _, bflags, _, _, _, _, dim_b, _, _, _, _ = BIND_STRUCT.unpack_from(m, bo + (b0 + j) * bsize)
                if (dim_b & 7) == 5 and desc == 1:
                    flags |= F_BUF_COMBINED
                if bflags & VK['BF_RW']:
                    flags |= F_BF_RW
                if kind == kind_tbuf or desc in (4, 5):
                    flags |= F_TBUF
                if kind in kind_dyn:
                    flags |= F_BINDLESS
                if kind in kind_gmem:
                    flags |= F_GMEM_SBUF
            if flags & F_BUF_COMBINED:
                counts['buf_combined'] += 1
            if flags & F_BF_RW:
                counts['bf_rw'] += 1
            if flags & F_TBUF:
                counts['tbuf'] += 1
            if flags & F_BINDLESS:
                counts['bindless'] += 1
            if flags & F_GMEM_SBUF:
                counts['gmem_sbuf'] += 1
            key_flags[ks] = flags

        return LibraryScan(
            path=lp,
            version=L.version,
            chain=L.chain,
            game_id=L.game,
            inputs=L.inputs,
            total=L.ne,
            key_flags=key_flags,
            counts=counts,
        )
    finally:
        L.close()


def parse_custom_keys(text: str) -> set[str]:
    """Extract 16-hex-digit shader keys from pasted log text or key lists."""
    return {m.lower() for m in KEY_RE.findall(text or '')}


def select_rebuild_keys(
    scan: LibraryScan,
    filter_mask: int,
    custom_keys: set[str] | None = None,
    rebuild_all: bool = False,
    ps_to_vs: dict[str, set[str]] | None = None,
) -> tuple[set[str], set[str]]:
    """Return (rebuild_keys, keep_keys) based on a bitmask of F_* flags, explicit keys, and optional PS->VS pairs."""
    if rebuild_all:
        return set(scan.key_flags), set()
    extra = custom_keys or set()
    rebuild = {k for k, fl in scan.key_flags.items() if (fl & filter_mask) or (k in extra)}
    if ps_to_vs:
        paired_vs: set[str] = set()
        for k in rebuild:
            if scan.key_flags.get(k, 0) & F_PS:
                paired_vs.update(vs for vs in ps_to_vs.get(k, ()) if vs in scan.key_flags)
        rebuild.update(paired_vs)
    keep = set(scan.key_flags) - rebuild
    return rebuild, keep


def _try_copy_r(src: Path, dst: Path, target_vk: str, target_inputs: str, val_id: str, expected_vk: str | None = None) -> bool:
    """Fast-path: reuse a matching .r file by updating only its 'vk' (and 'reused') stamp fields."""
    try:
        with open(src, 'rb') as f:
            hdr = f.read(12)
            if len(hdr) != 12 or hdr[:8] != RESULT_MAGIC:
                return False
            n = struct.unpack('<I', hdr[8:])[0]
            jbytes = f.read(n)
            if len(jbytes) != n:
                return False
            h = json.loads(jbytes)
            body = f.read()
        if (h.get('inputs'), h.get('raw'), h.get('val'), h.get('st')) != (target_inputs, False, val_id, 'ok'):
            return False
        if expected_vk is not None and h.get('vk') != expected_vk:
            return False
        if sum(m[2] for m in h['modules']) != len(body):
            return False
    except (OSError, ValueError, KeyError, TypeError):
        return False

    h['vk'] = target_vk
    if expected_vk is not None and expected_vk != target_vk:
        h['reused'] = expected_vk
    new_j = json.dumps(h, separators=(',', ':')).encode()
    tmp = dst.with_name(dst.name + f'.{os.getpid()}.tmp')
    with open(tmp, 'wb') as f:
        f.write(RESULT_MAGIC + struct.pack('<I', len(new_j)) + new_j + body)
    os.replace(tmp, dst)
    return True


def _is_valid_target_r(path: Path, target_vk: str, target_inputs: str, val_id: str, expected_reused: str | None = None) -> bool:
    try:
        with open(path, 'rb') as f:
            hdr = f.read(12)
            if len(hdr) != 12 or hdr[:8] != RESULT_MAGIC:
                return False
            n = struct.unpack('<I', hdr[8:])[0]
            h = json.loads(f.read(n))
            size = os.fstat(f.fileno()).st_size
        if expected_reused is not None and h.get('reused') != expected_reused:
            return False
        return (
            (h.get('vk'), h.get('inputs'), h.get('raw'), h.get('val'), h.get('key'))
            == (target_vk, target_inputs, False, val_id, path.stem)
            and 12 + n + sum(m[2] for m in h['modules']) == size
        )
    except (OSError, ValueError, KeyError, TypeError):
        return False


def migrate_to_results(
    lib_path: str | Path,
    res_dir: str | Path,
    target_vk: str,
    target_inputs: str,
    val_id: str,
    keep_keys: set[str],
    results_dirs: list[Path] | tuple[Path, ...] = (),
    progress_cb=None,
    stop_flag=None,
) -> dict[str, int]:
    """Populate <res_dir>/<key>.r for every key in keep_keys without re-translating.

    Reuses matching .r files from lib_path's own version when compatible, otherwise extracts
    the already-compressed entry directly from dmsh_vk.lib using vk_android_delta's reused format.
    """
    lib_p = Path(lib_path)
    res_p = Path(res_dir)
    res_p.mkdir(parents=True, exist_ok=True)
    L = delta.Library(str(lib_p))
    stats = {'already': 0, 'from_r': 0, 'from_lib': 0, 'total': len(keep_keys)}
    done = 0
    last_cb = 0.0
    try:
        old_ver = L.version
        # Only trust cached .r files belonging strictly to lib_p's own directory (never sibling .stale-backup or other v-* dirs)
        own_res = lib_p.parent / 'results'
        res_dirs = [own_res] if own_res.is_dir() else []
        expected_reused = None if old_ver == target_vk else old_ver
        for ks in sorted(keep_keys):
            if stop_flag and stop_flag():
                raise InterruptedError('Migration cancelled by user')
            idx = L.index.get(ks)
            if idx is None:
                continue
            dst = res_p / f'{ks}.r'
            if dst.is_file() and _is_valid_target_r(dst, target_vk, target_inputs, val_id, expected_reused=expected_reused):
                stats['already'] += 1
            else:
                copied = False
                for rd in res_dirs:
                    src_r = rd / f'{ks}.r'
                    if src_r.is_file() and src_r.resolve() != dst.resolve():
                        if _try_copy_r(src_r, dst, target_vk, target_inputs, val_id, expected_vk=old_ver):
                            copied = True
                            stats['from_r'] += 1
                            break
                if not copied:
                    _, e, b, c1, mods = L.entry(idx)
                    st_num = struct.unpack_from('<B', e, 16)[0]
                    stage = STAGE_NAME.get(st_num, 'ps')
                    refl = {
                        'stage': stage,
                        'reused': old_ver,
                        'lib': {'entry': delta.zeroed(e).hex(), 'binds': b.hex(), 'c1': c1.hex()},
                    }
                    h = {
                        'vk': target_vk,
                        'inputs': target_inputs,
                        'raw': False,
                        'val': val_id,
                        'key': ks,
                        'stage': stage,
                        'st': 'ok',
                        'err': '',
                        'ps_inputs': None,
                        'refl': refl,
                        'modules': [[v, size, len(stored), codec] for v, size, stored, codec in mods],
                        'reused': old_ver,
                    }
                    jbytes = json.dumps(h, separators=(',', ':')).encode()
                    tmp = dst.with_name(dst.name + f'.{os.getpid()}.tmp')
                    with open(tmp, 'wb') as f:
                        f.write(RESULT_MAGIC + struct.pack('<I', len(jbytes)) + jbytes)
                        for _, _, stored, _ in mods:
                            f.write(stored)
                    os.replace(tmp, dst)
                    stats['from_lib'] += 1
            done += 1
            now = time.monotonic()
            if progress_cb and (now - last_cb >= 0.1 or done == stats['total']):
                last_cb = now
                progress_cb(done, stats['total'], stats)
    finally:
        L.close()
    return stats


def _clean_stale_for_rebuild(res_dir: Path, rebuild_keys: set[str]):
    """Remove any existing .r files in target res_dir for keys marked for rebuild."""
    for ks in rebuild_keys:
        p = res_dir / f'{ks}.r'
        if p.is_file():
            p.unlink()


def sync_to_pack_manual(work_dir: Path, pack_dir: Path, title_id: str, target_vk: str):
    """Copy v-<target_vk>/dmsh_vk.lib and inputs.tsv into android-pack/shaders/ and update pack.tsv if present."""
    src_dir = work_dir / 'android-shaders' / 'lib' / f'v-{target_vk}'
    dst_dir = pack_dir / 'shaders' / f'v-{target_vk}'
    dst_dir.mkdir(parents=True, exist_ok=True)
    for fname in (VK['LIB_FILE'], dmsh_chain.STAMP):
        s = src_dir / fname
        d = dst_dir / fname
        if s.is_file():
            tmp = d.with_name(fname + '.partial')
            shutil.copyfile(s, tmp)
            os.replace(tmp, d)

    pack_tsv = pack_dir / 'pack.tsv'
    if pack_tsv.is_file():
        lines = pack_tsv.read_text(encoding='utf-8').splitlines()
        new_row = f'shaders\tshaders/v-{target_vk}/{VK["LIB_FILE"]}'
        out_lines = []
        replaced = False
        for line in lines:
            if line.startswith('shaders\t'):
                out_lines.append(new_row)
                replaced = True
            else:
                out_lines.append(line)
        if not replaced:
            out_lines.append(new_row)
        tmp = pack_tsv.with_name('pack.tsv.partial')
        tmp.write_text('\n'.join(out_lines) + '\n', encoding='utf-8', newline='\n')
        os.replace(tmp, pack_tsv)


def build_vk_command(ctx: GameContext, threads: int, require_all: bool = True) -> tuple[list[str], dict[str, str]]:
    """Build command and environment for vk_android_win.py (cross-platform dotnet/spirv-val)."""
    bananative_env.apply()
    env = os.environ.copy()
    env['DM_GAME'] = ctx.title_id
    env['J'] = str(max(1, threads))
    swork = ctx.work_dir / 'android-shaders' / 'work' / f'v-{ctx.target_vk}'
    swork.mkdir(parents=True, exist_ok=True)
    env['WORK'] = str(swork)
    env['DMSH_VK_REQUIRE_ALL'] = '1' if require_all else '0'
    ctx.lib_dir.mkdir(parents=True, exist_ok=True)
    cmd = [sys.executable, str(VK_BUILD_WIN), '--target', 'vulkan', str(ctx.pkg_dir), str(ctx.lib_dir)]
    return cmd, env


def build_pack_command(ctx: GameContext, threads: int, version: int | None = None, keep_code: bool = False) -> tuple[list[str], dict[str, str]]:
    """Build command for scripts/pack_android.py (handles first-time split, code rewrite, shader build, pack.tsv)."""
    if not ctx.game_source:
        raise RuntimeError('Select a dump folder or .nup file; pack_android.py requires it')
    bananative_env.apply()
    env = os.environ.copy()
    cmd = [sys.executable, str(PACK_ANDROID), str(ctx.game_source), '--out', str(ctx.pack_dir), '-J', str(max(1, threads))]
    if version is not None:
        cmd += ['--version', str(version)]
    if keep_code:
        cmd.append('--keep-code')
    return cmd, env


def _cli():
    ap = argparse.ArgumentParser(description='Fast incremental shader rebuild (from older dmsh_vk.lib / results) or full shader build')
    ap.add_argument('target', help='Dump folder, .nup file, or <titleID>-work folder')
    ap.add_argument('--mode', choices=('fast', 'full'), default='fast', help='fast: migrate unaffected shaders from older lib; full: normal full build via pack_android.py / vk_android_win.py')
    ap.add_argument('--from-lib', type=Path, help='Older dmsh_vk.lib to migrate from (default: newest older library in work dir)')
    ap.add_argument('--cs', action='store_true', help='Rebuild compute shaders')
    ap.add_argument('--buf-combined', action='store_true', help='Rebuild shaders with Buffer + COMBINED_IMAGE_SAMPLER bindings')
    ap.add_argument('--rw', action='store_true', help='Rebuild shaders with writable storage/image bindings (BF_RW)')
    ap.add_argument('--tbuf', action='store_true', help='Rebuild shaders with texel buffer bindings')
    ap.add_argument('--bindless', action='store_true', help='Rebuild shaders using dynamic/pool bindless bindings')
    ap.add_argument('--gmem', action='store_true', help='Rebuild shaders using storage buffer / gmem_map bindings')
    ap.add_argument('--stages', default='', help='Comma-separated extra stages to rebuild (e.g. vs,ps,gs,ms)')
    ap.add_argument('--keys', type=Path, help='File containing hex shader keys to rebuild')
    ap.add_argument('-J', '--threads', type=int, default=min(4, os.cpu_count() or 4), help='Parallel translator jobs')
    ap.add_argument('--no-sync-pack', action='store_true', help='Do not copy built library into android-pack/shaders/')
    ap.add_argument('--dry-run', action='store_true', help='Only print counts without running build')
    args = ap.parse_args()

    try:
        ctx = inspect_source(args.target)
    except Exception as e:
        sys.exit(f'[RED] {e}')
    if ctx.profile_error:
        sys.exit(f'[RED] {ctx.profile_error}')

    print(f'Game: {ctx.title_id} {ctx.game_name} | target vk: {ctx.target_vk} | pkg ready: {ctx.pkg_exists}')

    if args.mode == 'full':
        if ctx.game_source:
            cmd, env = build_pack_command(ctx, args.threads)
        elif ctx.pkg_exists:
            cmd, env = build_vk_command(ctx, args.threads)
        else:
            sys.exit('[RED] Tidak ada sumber dump/.nup maupun folder pkg/ untuk build full.')
        print('CMD:', ' '.join(cmd))
        if args.dry_run:
            return
        r = subprocess.run(cmd, env=env)
        if r.returncode:
            sys.exit(r.returncode)
        if not ctx.game_source and not args.no_sync_pack:
            sync_to_pack_manual(ctx.work_dir, ctx.pack_dir, ctx.title_id, ctx.target_vk)
        return

    src_lib = args.from_lib or next((p for p in ctx.old_libs), None)
    if not src_lib or not src_lib.is_file():
        sys.exit('[RED] Tidak ditemukan dmsh_vk.lib lama di work folder. Gunakan --mode full atau --from-lib.')

    if not ctx.pkg_exists:
        if not ctx.game_source:
            sys.exit('[RED] Folder pkg/ belum ada dan sumber dump/.nup tidak diketahui.')
        print('pkg/ belum ada, menjalankan split & rewrite kode terlebih dahulu (--no-shaders)...')
        if not args.dry_run:
            cmd_split, env_split = build_pack_command(ctx, args.threads)
            cmd_split.append('--no-shaders')
            r = subprocess.run(cmd_split, env=env_split)
            if r.returncode:
                sys.exit(r.returncode)

    scan = scan_library(src_lib)
    fmask = 0
    if args.cs: fmask |= F_CS
    if args.buf_combined: fmask |= F_BUF_COMBINED
    if args.rw: fmask |= F_BF_RW
    if args.tbuf: fmask |= F_TBUF
    if args.bindless: fmask |= F_BINDLESS
    if args.gmem: fmask |= F_GMEM_SBUF
    for s in args.stages.split(','):
        s = s.strip().lower()
        if s in STAGE_FLAG_MAP:
            fmask |= STAGE_FLAG_MAP[s]
    ckeys = parse_custom_keys(args.keys.read_text(encoding='utf-8')) if args.keys else set()
    rebuild_keys, keep_keys = select_rebuild_keys(scan, fmask, ckeys)

    print(f'Source lib: {src_lib} (v-{scan.version}, {scan.total:,} shaders)')
    print(f'Skip/migrate: {len(keep_keys):,} | Rebuild: {len(rebuild_keys):,} ({100.0 * len(rebuild_keys) / max(1, scan.total):.1f}%)')
    if args.dry_run:
        return

    res_dir = ctx.lib_dir / f'v-{ctx.target_vk}' / 'results'
    _clean_stale_for_rebuild(res_dir, rebuild_keys)
    st = migrate_to_results(
        src_lib, res_dir, ctx.target_vk, ctx.target_inputs, ctx.val_id, keep_keys,
        results_dirs=ctx.old_results_dirs,
        progress_cb=lambda d, t, s: print(f'migrate {d}/{t} (r:{s["from_r"]} lib:{s["from_lib"]} cached:{s["already"]})', flush=True) if d == t or d % 10000 == 0 else None,
    )
    print(f'Migrasi selesai: {st}')

    cmd, env = build_vk_command(ctx, args.threads)
    print('Running:', ' '.join(cmd))
    r = subprocess.run(cmd, env=env)
    if r.returncode:
        sys.exit(r.returncode)

    if not args.no_sync_pack:
        if ctx.game_source and (ctx.pack_dir / 'main.android.key').is_file():
            pk_cmd, pk_env = build_pack_command(ctx, args.threads, keep_code=True)
            subprocess.run(pk_cmd, env=pk_env, check=True)
        sync_to_pack_manual(ctx.work_dir, ctx.pack_dir, ctx.title_id, ctx.target_vk)
        print(f'Pack diperbarui: {ctx.pack_dir / "shaders" / f"v-{ctx.target_vk}"}')


if __name__ == '__main__':
    _cli()
