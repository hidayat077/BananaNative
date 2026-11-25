#!/usr/bin/env python3
"""PySide6 GUI for fast incremental shader rebuilds and first-time full shader builds.

Usage: python3 scripts/shader_rebuild_gui.py [<game .nup | dump folder | -work dir>]
"""
from __future__ import annotations

import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

from PySide6 import QtCore, QtGui, QtWidgets

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / 'scripts'))
import shader_fast_rebuild as sfr  # noqa: E402

RE_PROGRESS = re.compile(r'^progress\s+(\d+)\s+(\d+)\s*(.*)$')
RE_STAGE = re.compile(r'^stage:\s*(.+)$')
WINDOWS = os.name == 'nt'


class BuildWorker(QtCore.QThread):
    log_line = QtCore.Signal(str, bool)      # text, is_red
    stage_changed = QtCore.Signal(str)
    progress_changed = QtCore.Signal(int, int, str)
    finished_ok = QtCore.Signal(bool, str)

    def __init__(self, params: dict):
        super().__init__()
        self.params = params
        self._stop = False
        self._proc: subprocess.Popen | None = None

    def request_stop(self):
        self._stop = True
        p = self._proc
        if p and p.poll() is None:
            try:
                if WINDOWS:
                    p.terminate()
                else:
                    os.killpg(os.getpgid(p.pid), signal.SIGTERM)
            except OSError:
                p.kill()

    def _run_subprocess(self, cmd: list[str], env: dict[str, str], phase_label: str) -> int:
        self.log_line.emit(f'$ {" ".join(cmd)}', False)
        kw = {'creationflags': subprocess.CREATE_NEW_PROCESS_GROUP} if WINDOWS else {'start_new_session': True}
        self._proc = subprocess.Popen(
            cmd,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors='replace',
            bufsize=1,
            **kw,
        )
        t0 = time.monotonic()
        base_done = None
        assert self._proc.stdout is not None
        for raw in self._proc.stdout:
            line = raw.rstrip('\r\n')
            is_red = '[RED]' in line or 'Error' in line or 'Traceback' in line
            self.log_line.emit(line, is_red)
            if m := RE_STAGE.match(line):
                self.stage_changed.emit(f'{phase_label}: {m.group(1)}')
            elif m := RE_PROGRESS.match(line):
                done, total, st = int(m.group(1)), max(1, int(m.group(2))), m.group(3).strip()
                if base_done is None:
                    base_done = done
                elapsed = max(0.1, time.monotonic() - t0)
                built_now = max(0, done - base_done)
                rate = built_now / elapsed
                eta = ''
                if st == 'translate' and rate > 0.05 and done < total:
                    rem = int((total - done) / rate)
                    eta = f' | ~{rem // 60}m {rem % 60:02d}s left ({rate:.1f}/s)'
                self.progress_changed.emit(done, total, f'{st or phase_label}: {done:,} / {total:,}{eta}')
            if self._stop:
                break
        rc = self._proc.wait()
        self._proc = None
        return rc

    def _resolve_ooz(self, ctx: sfr.GameContext) -> Path | None:
        p = self.params
        if not p.get('use_ooz'):
            return None
        raw = (p.get('ooz_path') or '').strip()
        if raw and Path(raw).is_file():
            return Path(raw)
        found = sfr.find_ooz(ctx.work_dir)
        if found:
            return found
        exe = 'unoodle.exe' if WINDOWS else 'unoodle'
        target = ctx.work_dir / 'tools' / exe
        self.stage_changed.emit('Building ooz Oodle decoder (unoodle)...')
        self.log_line.emit(f'$ sh {sfr.OOZ_BUILD_SH} {target}', False)
        ok, out = sfr.build_ooz(target)
        if out:
            for line in out.splitlines():
                self.log_line.emit(line, not ok)
        if not ok:
            raise RuntimeError(f'Failed to build ooz Oodle decoder: {out}')
        self.log_line.emit(f'ooz ready: {target}', False)
        return target

    def run(self):
        p = self.params
        ctx: sfr.GameContext = p['ctx']
        mode: str = p['mode']
        threads: int = p['threads']
        try:
            if mode == 'full':
                self.stage_changed.emit('Running full build (Normal Mode)...')
                if p['use_pack_android'] and ctx.game_source:
                    ooz_path = self._resolve_ooz(ctx)
                    cmd, env = sfr.build_pack_command(ctx, threads, keep_code=p['keep_code'], ooz_path=ooz_path)
                    rc = self._run_subprocess(cmd, env, 'Pack & Build')
                else:
                    if not ctx.pkg_exists:
                        raise RuntimeError('pkg/ folder not found. Select a .nup file or dump folder for initial split.')
                    cmd, env = sfr.build_vk_command(ctx, threads, require_all=p['require_all'])
                    rc = self._run_subprocess(cmd, env, 'Shader Build')
                    if rc == 0 and p['sync_pack'] and not self._stop:
                        self.stage_changed.emit('Copying library to android-pack/shaders/...')
                        sfr.sync_to_pack_manual(ctx.work_dir, ctx.pack_dir, ctx.title_id, ctx.target_vk)
                if self._stop:
                    self.finished_ok.emit(False, 'Build cancelled.')
                elif rc != 0:
                    self.finished_ok.emit(False, f'Build failed (exit code {rc}).')
                else:
                    self.finished_ok.emit(True, f'Full build complete -> {ctx.pack_dir / "shaders" / f"v-{ctx.target_vk}"}')
                return

            # Fast Hack Incremental Rebuild
            if not ctx.pkg_exists:
                if not ctx.game_source:
                    raise RuntimeError('pkg/ folder missing and no dump/.nup source selected.')
                self.stage_changed.emit('pkg/ missing: splitting dump & rewriting code first...')
                ooz_path = self._resolve_ooz(ctx)
                cmd_sp, env_sp = sfr.build_pack_command(ctx, threads, ooz_path=ooz_path)
                cmd_sp.append('--no-shaders')
                rc = self._run_subprocess(cmd_sp, env_sp, 'Split & Code')
                if rc != 0:
                    self.finished_ok.emit(False, f'Split failed (exit code {rc}).')
                    return
                ctx = sfr.inspect_source(ctx.input_path)

            src_lib: Path = p['src_lib']
            rebuild_keys: set[str] = p['rebuild_keys']
            keep_keys: set[str] = p['keep_keys']
            res_dir = ctx.lib_dir / f'v-{ctx.target_vk}' / 'results'

            self.stage_changed.emit(f'Phase 1/2: Migrating {len(keep_keys):,} unchanged shaders to results/...')
            self.log_line.emit(f'Source library: {src_lib}', False)
            sfr._clean_stale_for_rebuild(res_dir, rebuild_keys)
            st = sfr.migrate_to_results(
                src_lib,
                res_dir,
                ctx.target_vk,
                ctx.target_inputs,
                ctx.val_id,
                keep_keys,
                results_dirs=ctx.old_results_dirs,
                progress_cb=lambda d, t, s: self.progress_changed.emit(
                    d, max(1, t), f'Migrating cache: {d:,}/{t:,} (r:{s["from_r"]:,} lib:{s["from_lib"]:,} hit:{s["already"]:,})'
                ),
                stop_flag=lambda: self._stop,
            )
            self.log_line.emit(
                f'Migration done -> reused .r: {st["from_r"]:,}, extracted from lib: {st["from_lib"]:,}, already cached: {st["already"]:,}',
                False,
            )

            if self._stop:
                self.finished_ok.emit(False, 'Build cancelled during migration.')
                return

            self.stage_changed.emit(f'Phase 2/2: Rebuilding {len(rebuild_keys):,} selected shaders...')
            cmd, env = sfr.build_vk_command(ctx, threads, require_all=p['require_all'])
            rc = self._run_subprocess(cmd, env, 'Incremental Build')
            if self._stop:
                self.finished_ok.emit(False, 'Build cancelled.')
                return
            if rc != 0:
                self.finished_ok.emit(False, f'vk_android_win.py failed (exit code {rc}).')
                return

            if p['sync_pack']:
                self.stage_changed.emit('Copying to android-pack and updating pack.tsv...')
                if ctx.game_source and (ctx.pack_dir / 'main.android.key').is_file():
                    pk_cmd, pk_env = sfr.build_pack_command(ctx, threads, keep_code=True)
                    pk_rc = self._run_subprocess(pk_cmd, pk_env, 'Update Pack')
                    if pk_rc != 0:
                        self.finished_ok.emit(False, f'Pack update failed (exit code {pk_rc}).')
                        return
                sfr.sync_to_pack_manual(ctx.work_dir, ctx.pack_dir, ctx.title_id, ctx.target_vk)

            self.finished_ok.emit(True, f'Fast rebuild complete! Output: {ctx.pack_dir / "shaders" / f"v-{ctx.target_vk}"}')
        except Exception as e:
            self.log_line.emit(f'[RED] {e}', True)
            self.finished_ok.emit(False, str(e))


class ShaderRebuildWindow(QtWidgets.QMainWindow):
    def __init__(self, initial: str | None = None):
        super().__init__()
        self.setWindowTitle('Polar — Shader Rebuild Studio')
        self.setMinimumSize(880, 680)
        self.resize(1040, 820)
        self.ctx: sfr.GameContext | None = None
        self.scan: sfr.LibraryScan | None = None
        self.ps_to_vs: dict[str, set[str]] = {}
        self.worker: BuildWorker | None = None

        self._apply_theme()

        central = QtWidgets.QWidget()
        self.setCentralWidget(central)
        outer = QtWidgets.QVBoxLayout(central)
        outer.setContentsMargins(14, 12, 14, 12)
        outer.setSpacing(10)

        self.tabs = QtWidgets.QTabWidget()
        outer.addWidget(self.tabs)

        builder_tab = QtWidgets.QWidget()
        root = QtWidgets.QVBoxLayout(builder_tab)
        root.setContentsMargins(10, 12, 10, 10)
        root.setSpacing(10)
        self.tabs.addTab(builder_tab, 'Builder')

        # Scrollable top configuration pane so cards never crush the build log
        scroll = QtWidgets.QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QtWidgets.QFrame.Shape.NoFrame)
        cfg_host = QtWidgets.QWidget()
        cfg_lay = QtWidgets.QVBoxLayout(cfg_host)
        cfg_lay.setContentsMargins(0, 0, 4, 0)
        cfg_lay.setSpacing(10)
        scroll.setWidget(cfg_host)

        # 1. Source Picker
        src_box = QtWidgets.QGroupBox('1. Game / Work Folder')
        src_lay = QtWidgets.QVBoxLayout(src_box)
        src_lay.setSpacing(8)
        src_row = QtWidgets.QHBoxLayout()
        src_row.setSpacing(8)
        self.path_edit = QtWidgets.QLineEdit()
        self.path_edit.setPlaceholderText('Select a .nup file, dump folder (base/, updates/), or <titleID>-work folder...')
        btn_nup = QtWidgets.QPushButton('.nup File…')
        btn_dir = QtWidgets.QPushButton('Folder…')
        btn_load = QtWidgets.QPushButton('Load')
        btn_load.setObjectName('secondaryBtn')
        btn_nup.clicked.connect(self.pick_nup)
        btn_dir.clicked.connect(self.pick_dir)
        btn_load.clicked.connect(lambda: self.load_target(self.path_edit.text().strip()))
        src_row.addWidget(self.path_edit, 1)
        src_row.addWidget(btn_nup)
        src_row.addWidget(btn_dir)
        src_row.addWidget(btn_load)
        src_lay.addLayout(src_row)

        self.info_lbl = QtWidgets.QLabel('No game selected.')
        self.info_lbl.setWordWrap(True)
        self.info_lbl.setTextInteractionFlags(QtCore.Qt.TextInteractionFlag.TextSelectableByMouse)
        self.info_lbl.setObjectName('infoBox')
        src_lay.addWidget(self.info_lbl)
        cfg_lay.addWidget(src_box)

        # 2. Mode + Old Source
        mode_box = QtWidgets.QGroupBox('2. Build Mode')
        mode_lay = QtWidgets.QVBoxLayout(mode_box)
        mode_lay.setSpacing(8)
        top_mode = QtWidgets.QHBoxLayout()
        top_mode.setSpacing(18)
        self.rb_fast = QtWidgets.QRadioButton('Fast Hack Rebuild (skip unaffected shaders from an older library)')
        self.rb_full = QtWidgets.QRadioButton('First-Time / Full Build (split && translate all shaders)')
        self.rb_fast.toggled.connect(self._update_mode_ui)
        top_mode.addWidget(self.rb_fast)
        top_mode.addWidget(self.rb_full)
        top_mode.addStretch()
        mode_lay.addLayout(top_mode)

        self.old_row_widget = QtWidgets.QWidget()
        old_row = QtWidgets.QHBoxLayout(self.old_row_widget)
        old_row.setContentsMargins(0, 0, 0, 0)
        old_row.setSpacing(8)
        old_row.addWidget(QtWidgets.QLabel('Source library:'))
        self.lib_combo = QtWidgets.QComboBox()
        self.lib_combo.currentIndexChanged.connect(self._on_lib_selected)
        old_row.addWidget(self.lib_combo, 1)
        btn_browse_lib = QtWidgets.QPushButton('Browse .lib…')
        btn_browse_lib.clicked.connect(self.pick_old_lib)
        old_row.addWidget(btn_browse_lib)
        mode_lay.addWidget(self.old_row_widget)

        self.full_opts = QtWidgets.QWidget()
        fo_lay = QtWidgets.QHBoxLayout(self.full_opts)
        fo_lay.setContentsMargins(0, 0, 0, 0)
        fo_lay.setSpacing(16)
        self.chk_pack_android = QtWidgets.QCheckBox('Run via pack_android.py (split + code rewrite + shaders + pack.tsv)')
        self.chk_pack_android.setChecked(True)
        self.chk_pack_android.setToolTip('Recommended for first-time setup or full pack refresh from a .nup or dump folder.')
        self.chk_keep_code = QtWidgets.QCheckBox('Keep rewritten code (--keep-code)')
        self.chk_keep_code.setToolTip('Skip NSO code rewriting if main.patched.bin is already up to date.')
        fo_lay.addWidget(self.chk_pack_android)
        fo_lay.addWidget(self.chk_keep_code)
        fo_lay.addStretch()
        mode_lay.addWidget(self.full_opts)

        ooz_row = QtWidgets.QHBoxLayout()
        ooz_row.setSpacing(8)
        self.chk_ooz = QtWidgets.QCheckBox('Use ooz Oodle decoder (DM_OODLE_OOZ)')
        self.chk_ooz.setToolTip('Enable for Oodle-compressed games (e.g. Unreal, Trinity, PKZ) when the profile has shader_map.tsv.')
        self.ooz_edit = QtWidgets.QLineEdit()
        self.ooz_edit.setPlaceholderText('Auto-build via dm/third_party/ooz/build.sh when needed...')
        self.btn_build_ooz = QtWidgets.QPushButton('Build ooz')
        self.btn_build_ooz.clicked.connect(self.build_ooz_now)
        btn_browse_ooz = QtWidgets.QPushButton('Browse…')
        btn_browse_ooz.clicked.connect(self.pick_ooz)
        ooz_row.addWidget(self.chk_ooz)
        ooz_row.addWidget(self.ooz_edit, 1)
        ooz_row.addWidget(self.btn_build_ooz)
        ooz_row.addWidget(btn_browse_ooz)
        mode_lay.addLayout(ooz_row)
        cfg_lay.addWidget(mode_box)

        # 3. Simplified Preset + Auto-detect + Collapsible Advanced Filters
        self.filter_box = QtWidgets.QGroupBox('3. What Needs Rebuilding? (Fast Mode)')
        flt_lay = QtWidgets.QVBoxLayout(self.filter_box)
        flt_lay.setSpacing(8)

        preset_row = QtWidgets.QHBoxLayout()
        preset_row.setSpacing(8)
        preset_row.addWidget(QtWidgets.QLabel('Preset:'))
        self.preset_combo = QtWidgets.QComboBox()
        for key, (label, _) in sfr.PRESETS.items():
            self.preset_combo.addItem(label, key)
        self.preset_combo.currentIndexChanged.connect(self._on_preset_changed)
        preset_row.addWidget(self.preset_combo, 1)
        self.btn_rescan_git = QtWidgets.QPushButton('Re-scan Git')
        self.btn_rescan_git.clicked.connect(self._apply_git_autodetect)
        preset_row.addWidget(self.btn_rescan_git)
        flt_lay.addLayout(preset_row)

        self.preset_note_lbl = QtWidgets.QLabel('')
        self.preset_note_lbl.setWordWrap(True)
        flt_lay.addWidget(self.preset_note_lbl)

        custom_row = QtWidgets.QHBoxLayout()
        custom_row.setSpacing(8)
        custom_row.addWidget(QtWidgets.QLabel('Paste error log or shader keys:'))
        self.custom_edit = QtWidgets.QLineEdit()
        self.custom_edit.setPlaceholderText('Paste [RED] log lines or 16-hex shader IDs (auto-extracted)...')
        self.custom_edit.textChanged.connect(self._update_counts)
        custom_row.addWidget(self.custom_edit, 1)
        flt_lay.addLayout(custom_row)

        self.chk_pair_vs = QtWidgets.QCheckBox('Also rebuild paired VS when PS is selected (keeps VS output padding in sync)')
        self.chk_pair_vs.setToolTip('If a pixel shader changes its inputs, its paired vertex shaders need padding to match.')
        self.chk_pair_vs.toggled.connect(self._update_counts)
        flt_lay.addWidget(self.chk_pair_vs)

        self.btn_adv = QtWidgets.QToolButton()
        self.btn_adv.setText('Show Advanced Filters (9 manual checkboxes)')
        self.btn_adv.setCheckable(True)
        self.btn_adv.setChecked(False)
        self.btn_adv.setArrowType(QtCore.Qt.ArrowType.RightArrow)
        self.btn_adv.setToolButtonStyle(QtCore.Qt.ToolButtonStyle.ToolButtonTextBesideIcon)
        self.btn_adv.toggled.connect(self._toggle_advanced)
        flt_lay.addWidget(self.btn_adv)

        self.adv_panel = QtWidgets.QWidget()
        self.adv_panel.setVisible(False)
        grid = QtWidgets.QGridLayout(self.adv_panel)
        grid.setContentsMargins(4, 4, 4, 4)
        grid.setHorizontalSpacing(14)
        grid.setVerticalSpacing(6)
        self.chk_cs = QtWidgets.QCheckBox('Compute shaders (cs)')
        self.chk_cs.setToolTip('Rebuild all compute shaders (e.g. after AstOptimizer / compute lowering fixes).')
        self.chk_buf_comb = QtWidgets.QCheckBox('Buffer + COMBINED_IMAGE_SAMPLER')
        self.chk_buf_comb.setToolTip('Rebuild shaders binding Dim=Buffer as COMBINED_IMAGE_SAMPLER (buffer_textures_split).')
        self.chk_rw = QtWidgets.QCheckBox('Storage / Image Write (BF_RW)')
        self.chk_rw.setToolTip('Rebuild shaders with writable storage buffer or storage image bindings.')
        self.chk_tbuf = QtWidgets.QCheckBox('Texel Buffer (tbuf)')
        self.chk_tbuf.setToolTip('Rebuild shaders using UNIFORM_TEXEL_BUFFER or STORAGE_TEXEL_BUFFER.')
        self.chk_bindless = QtWidgets.QCheckBox('Bindless / Dynamic Slots (tex_dyn/pool)')
        self.chk_bindless.setToolTip('Rebuild shaders using bindless pools or runtime-indexed texture/sampler slots.')
        self.chk_gmem = QtWidgets.QCheckBox('Storage Buffer / GMEM map')
        self.chk_gmem.setToolTip('Rebuild shaders using storage buffers or raw global memory pool maps.')
        self.chk_vs = QtWidgets.QCheckBox('All Vertex (vs)')
        self.chk_vs.setToolTip('Rebuild every vertex shader.')
        self.chk_ps = QtWidgets.QCheckBox('All Pixel (ps)')
        self.chk_ps.setToolTip('Rebuild every pixel/fragment shader.')
        self.chk_gs_ms = QtWidgets.QCheckBox('Geometry / Tess / Mesh (gs/hs/ds/ts/ms)')
        self.chk_gs_ms.setToolTip('Rebuild all geometry, tessellation, task, and mesh shaders.')
        self.filter_checks = [
            (self.chk_cs, sfr.F_CS),
            (self.chk_buf_comb, sfr.F_BUF_COMBINED),
            (self.chk_rw, sfr.F_BF_RW),
            (self.chk_tbuf, sfr.F_TBUF),
            (self.chk_bindless, sfr.F_BINDLESS),
            (self.chk_gmem, sfr.F_GMEM_SBUF),
            (self.chk_vs, sfr.F_VS),
            (self.chk_ps, sfr.F_PS),
            (self.chk_gs_ms, sfr.F_GS | sfr.F_HS | sfr.F_DS | sfr.F_TS | sfr.F_MS),
        ]
        for idx, (cb, _) in enumerate(self.filter_checks):
            cb.toggled.connect(self._on_manual_checkbox)
            grid.addWidget(cb, idx // 3, idx % 3)
        flt_lay.addWidget(self.adv_panel)

        self.preview_lbl = QtWidgets.QLabel('Select or scan an older dmsh_vk.lib to preview shader counts.')
        self.preview_lbl.setStyleSheet('font-weight: 600; color: #93C5FD;')
        flt_lay.addWidget(self.preview_lbl)
        cfg_lay.addWidget(self.filter_box)
        cfg_lay.addStretch(1)

        # Bottom execution + log pane
        bottom_pane = QtWidgets.QWidget()
        bot_lay = QtWidgets.QVBoxLayout(bottom_pane)
        bot_lay.setContentsMargins(0, 4, 0, 0)
        bot_lay.setSpacing(8)

        ctrl_card = QtWidgets.QFrame()
        ctrl_card.setObjectName('ctrlCard')
        ctrl_row = QtWidgets.QHBoxLayout(ctrl_card)
        ctrl_row.setContentsMargins(12, 10, 12, 10)
        ctrl_row.setSpacing(12)
        ctrl_row.addWidget(QtWidgets.QLabel('Threads (J):'))
        self.spin_j = QtWidgets.QSpinBox()
        self.spin_j.setRange(1, max(16, (os.cpu_count() or 8) * 2))
        self.spin_j.setValue(min(4, os.cpu_count() or 4))
        self.spin_j.setMinimumWidth(64)
        ctrl_row.addWidget(self.spin_j)

        self.chk_sync_pack = QtWidgets.QCheckBox('Copy to android-pack && update pack.tsv')
        self.chk_sync_pack.setChecked(True)
        self.chk_req_all = QtWidgets.QCheckBox('DMSH_VK_REQUIRE_ALL=1')
        self.chk_req_all.setChecked(True)
        self.chk_req_all.setToolTip('Fail the build if any shader fails to translate or validate.')
        ctrl_row.addWidget(self.chk_sync_pack)
        ctrl_row.addWidget(self.chk_req_all)
        ctrl_row.addStretch()

        self.btn_start = QtWidgets.QPushButton('Start Build')
        self.btn_start.setObjectName('primaryBtn')
        self.btn_start.setMinimumWidth(120)
        self.btn_start.clicked.connect(self.start_build)
        self.btn_stop = QtWidgets.QPushButton('Stop')
        self.btn_stop.setObjectName('dangerBtn')
        self.btn_stop.setEnabled(False)
        self.btn_stop.clicked.connect(self.stop_build)
        self.btn_open = QtWidgets.QPushButton('Open Pack')
        self.btn_open.setEnabled(False)
        self.btn_open.clicked.connect(self.open_pack)
        self.btn_help = QtWidgets.QPushButton('? Guide')
        self.btn_help.clicked.connect(lambda: self.tabs.setCurrentIndex(1))
        ctrl_row.addWidget(self.btn_start)
        ctrl_row.addWidget(self.btn_stop)
        ctrl_row.addWidget(self.btn_open)
        ctrl_row.addWidget(self.btn_help)
        bot_lay.addWidget(ctrl_card)

        prog_row = QtWidgets.QHBoxLayout()
        prog_row.setSpacing(10)
        self.stage_lbl = QtWidgets.QLabel('Ready.')
        self.stage_lbl.setStyleSheet('font-weight: 600; color: #D1D5DB;')
        prog_row.addWidget(self.stage_lbl, 1)
        btn_clear_log = QtWidgets.QPushButton('Clear Log')
        btn_clear_log.setFixedHeight(26)
        btn_clear_log.clicked.connect(lambda: self.log_box.clear())
        prog_row.addWidget(btn_clear_log)
        bot_lay.addLayout(prog_row)

        self.pbar = QtWidgets.QProgressBar()
        self.pbar.setRange(0, 100)
        self.pbar.setFixedHeight(20)
        bot_lay.addWidget(self.pbar)

        self.log_box = QtWidgets.QPlainTextEdit()
        self.log_box.setReadOnly(True)
        self.log_box.setMinimumHeight(180)
        font = QtGui.QFont('Monospace', 9)
        font.setStyleHint(QtGui.QFont.StyleHint.TypeWriter)
        self.log_box.setFont(font)
        bot_lay.addWidget(self.log_box, 1)

        splitter = QtWidgets.QSplitter(QtCore.Qt.Orientation.Vertical)
        splitter.setChildrenCollapsible(False)
        splitter.addWidget(scroll)
        splitter.addWidget(bottom_pane)
        splitter.setStretchFactor(0, 0)
        splitter.setStretchFactor(1, 1)
        splitter.setSizes([380, 340])
        root.addWidget(splitter, 1)

        guide_tab = QtWidgets.QTextBrowser()
        guide_tab.setOpenExternalLinks(False)
        guide_tab.setHtml(self._guide_html())
        self.tabs.addTab(guide_tab, 'User Guide')

        if initial:
            self.path_edit.setText(initial)
            self.load_target(initial)
        else:
            self.rb_full.setChecked(True)
            self._update_mode_ui()
        self._on_preset_changed()

    def _apply_theme(self):
        self.setStyleSheet("""
            QMainWindow, QWidget {
                background-color: #11141B;
                color: #E5E7EB;
                font-size: 13px;
            }
            QTabWidget::pane {
                border: 1px solid #232A38;
                border-radius: 8px;
                background-color: #151922;
                top: -1px;
            }
            QTabBar::tab {
                background-color: #1A1F2B;
                color: #9CA3AF;
                border: 1px solid #232A38;
                border-bottom: none;
                border-top-left-radius: 8px;
                border-top-right-radius: 8px;
                padding: 8px 18px;
                margin-right: 4px;
                font-weight: 600;
            }
            QTabBar::tab:selected {
                background-color: #151922;
                color: #FFFFFF;
                border-color: #3B82F6;
            }
            QGroupBox {
                background-color: #1A1F2A;
                border: 1px solid #262E3D;
                border-radius: 8px;
                margin-top: 12px;
                padding: 14px 12px 12px 12px;
                font-weight: 600;
            }
            QGroupBox::title {
                subcontrol-origin: margin;
                subcontrol-position: top left;
                left: 12px;
                padding: 0 6px;
                color: #93C5FD;
                background-color: #151922;
                border-radius: 4px;
            }
            QFrame#ctrlCard {
                background-color: #1A1F2A;
                border: 1px solid #262E3D;
                border-radius: 8px;
            }
            QLabel#infoBox {
                background-color: #10131A;
                border: 1px solid #222938;
                border-radius: 6px;
                padding: 8px 10px;
                color: #D1D5DB;
            }
            QLineEdit, QComboBox, QSpinBox, QPlainTextEdit, QTextBrowser {
                background-color: #0E1117;
                color: #F3F4F6;
                border: 1px solid #2C3546;
                border-radius: 6px;
                padding: 6px 9px;
                selection-background-color: #2563EB;
            }
            QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QPlainTextEdit:focus {
                border-color: #3B82F6;
            }
            QLineEdit:disabled, QComboBox:disabled, QSpinBox:disabled {
                color: #6B7280;
                background-color: #141821;
            }
            QPushButton {
                background-color: #232A38;
                color: #E5E7EB;
                border: 1px solid #323C4F;
                border-radius: 6px;
                padding: 6px 14px;
                font-weight: 600;
            }
            QPushButton:hover {
                background-color: #2C3547;
                border-color: #4B5563;
            }
            QPushButton:pressed {
                background-color: #1E2430;
            }
            QPushButton:disabled {
                background-color: #181C26;
                color: #525B6C;
                border-color: #222836;
            }
            QPushButton#primaryBtn {
                background-color: #2563EB;
                color: #FFFFFF;
                border-color: #3B82F6;
            }
            QPushButton#primaryBtn:hover {
                background-color: #1D4ED8;
            }
            QPushButton#primaryBtn:disabled {
                background-color: #1E3A5F;
                color: #93C5FD;
                border-color: #1E3A5F;
            }
            QPushButton#dangerBtn:enabled {
                background-color: #991B1B;
                color: #FEE2E2;
                border-color: #DC2626;
            }
            QPushButton#dangerBtn:enabled:hover {
                background-color: #B91C1C;
            }
            QToolButton {
                background-color: #202634;
                color: #D1D5DB;
                border: 1px solid #2E3748;
                border-radius: 6px;
                padding: 5px 10px;
                font-weight: 600;
            }
            QToolButton:hover {
                background-color: #283042;
            }
            QProgressBar {
                background-color: #0E1117;
                border: 1px solid #262E3D;
                border-radius: 6px;
                text-align: center;
                color: #F3F4F6;
                font-weight: 600;
            }
            QProgressBar::chunk {
                background-color: #3B82F6;
                border-radius: 5px;
            }
            QSplitter::handle {
                background-color: #262E3D;
                height: 4px;
                margin: 2px 24px;
                border-radius: 2px;
            }
            QSplitter::handle:hover {
                background-color: #3B82F6;
            }
        """)

    def pick_nup(self):
        p, _ = QtWidgets.QFileDialog.getOpenFileName(self, 'Select .nup file', '', 'NUP (*.nup);;All Files (*)')
        if p:
            self.path_edit.setText(p)
            self.load_target(p)

    def pick_dir(self):
        p = QtWidgets.QFileDialog.getExistingDirectory(self, 'Select dump folder or -work folder')
        if p:
            self.path_edit.setText(p)
            self.load_target(p)

    def pick_old_lib(self):
        start = str(self.ctx.lib_dir) if self.ctx else ''
        p, _ = QtWidgets.QFileDialog.getOpenFileName(self, 'Select older dmsh_vk.lib', start, 'Shader Library (dmsh_vk.lib);;All Files (*)')
        if p:
            self.lib_combo.addItem(f'[Manual] {p}', Path(p))
            self.lib_combo.setCurrentIndex(self.lib_combo.count() - 1)

    def pick_ooz(self):
        start = str(self.ctx.work_dir) if self.ctx else ''
        p, _ = QtWidgets.QFileDialog.getOpenFileName(self, 'Select ooz / unoodle binary', start, 'All Files (*)')
        if p:
            self.ooz_edit.setText(p)
            if self.chk_ooz.isEnabled():
                self.chk_ooz.setChecked(True)

    def build_ooz_now(self):
        exe = 'unoodle.exe' if WINDOWS else 'unoodle'
        dst = (self.ctx.work_dir / 'tools' / exe) if self.ctx else (Path.cwd() / 'unoodle')
        self.append_log(f'$ sh {sfr.OOZ_BUILD_SH} {dst}', False)
        ok, out = sfr.build_ooz(dst)
        if out:
            for line in out.splitlines():
                self.append_log(line, not ok)
        if ok:
            self.ooz_edit.setText(str(dst))
            if self.chk_ooz.isEnabled():
                self.chk_ooz.setChecked(True)
            self.stage_lbl.setText(f'ooz ready: {dst}')
        else:
            QtWidgets.QMessageBox.warning(self, 'Build ooz failed', out)

    def load_target(self, path_str: str):
        if not path_str:
            return
        try:
            ctx = sfr.inspect_source(path_str)
        except Exception as e:
            QtWidgets.QMessageBox.critical(self, 'Error', str(e))
            return
        self.ctx = ctx
        self.scan = None
        self.ps_to_vs = sfr.load_ps_to_vs_pairs(ctx.title_id) if not ctx.profile_error else {}
        self.btn_open.setEnabled(ctx.pack_dir.is_dir())

        status_parts = [
            f'<b>{ctx.title_id} — {ctx.game_name}</b>',
            f'Target Vulkan chain: <code>v-{ctx.target_vk}</code> | spirv-val: <code>{ctx.val_id}</code>',
            f'Work: <code>{ctx.work_dir}</code> | pkg/ ready: <b>{"Yes" if ctx.pkg_exists else "No (will split)"}</b> | '
            f'v-{ctx.target_vk} built: <b>{"Yes (current)" if ctx.target_lib_current else ("File exists" if ctx.target_lib_exists else "No")}</b>',
        ]
        if ctx.profile_error:
            status_parts.append(f'<span style="color:#d03030;">{ctx.profile_error}</span>')
        self.info_lbl.setText('<br>'.join(status_parts))

        self.lib_combo.blockSignals(True)
        self.lib_combo.clear()
        for lp in ctx.old_libs:
            self.lib_combo.addItem(f'{lp.parent.name} ({lp})', lp)
        self.lib_combo.blockSignals(False)

        if ctx.old_libs:
            self.rb_fast.setChecked(True)
            self._on_lib_selected()
        else:
            self.rb_full.setChecked(True)

        has_map = sfr.has_shader_map(ctx.title_id)
        found_ooz = sfr.find_ooz(ctx.work_dir)
        if found_ooz and not self.ooz_edit.text().strip():
            self.ooz_edit.setText(str(found_ooz))
        self.chk_ooz.setEnabled(has_map)
        self.chk_ooz.setChecked(has_map)
        self.ooz_edit.setEnabled(has_map)
        self.btn_build_ooz.setEnabled(has_map and not WINDOWS)
        self._update_mode_ui()

    def _on_lib_selected(self):
        lp = self.lib_combo.currentData()
        if not lp or not Path(lp).is_file():
            self.scan = None
            self._update_counts()
            return
        try:
            self.scan = sfr.scan_library(lp)
            c = self.scan.counts
            self.chk_cs.setText(f'Compute shaders (cs: {c["cs"]:,})')
            self.chk_buf_comb.setText(f'Buffer + COMBINED_IMAGE_SAMPLER ({c["buf_combined"]:,})')
            self.chk_rw.setText(f'Storage / Image Write BF_RW ({c["bf_rw"]:,})')
            self.chk_tbuf.setText(f'Texel Buffer tbuf ({c["tbuf"]:,})')
            self.chk_bindless.setText(f'Bindless / Dyn Slots ({c["bindless"]:,})')
            self.chk_gmem.setText(f'Storage Buffer / GMEM ({c["gmem_sbuf"]:,})')
            self.chk_vs.setText(f'All Vertex (vs: {c["vs"]:,})')
            self.chk_ps.setText(f'All Pixel (ps: {c["ps"]:,})')
            other = c['gs'] + c['hs'] + c['ds'] + c['ts'] + c['ms']
            self.chk_gs_ms.setText(f'Geometry / Tess / Mesh ({other:,})')
        except Exception as e:
            self.scan = None
            self.preview_lbl.setText(f'Failed to scan library: {e}')
        if self.preset_combo.currentData() == 'auto':
            self._apply_git_autodetect()
        else:
            self._update_counts()

    def _selected_mask(self) -> int:
        mask = 0
        for cb, flag in self.filter_checks:
            if cb.isChecked():
                mask |= flag
        return mask

    def _set_mask(self, mask: int):
        for cb, flag in self.filter_checks:
            cb.blockSignals(True)
            cb.setChecked(bool(mask & flag))
            cb.blockSignals(False)
        self._update_counts()

    def _toggle_advanced(self, show: bool):
        self.adv_panel.setVisible(show)
        self.btn_adv.setArrowType(QtCore.Qt.ArrowType.DownArrow if show else QtCore.Qt.ArrowType.RightArrow)
        self.btn_adv.setText('Hide Advanced Filters' if show else 'Show Advanced Filters (9 manual checkboxes)')

    def _on_manual_checkbox(self):
        idx = self.preset_combo.findData('custom')
        if idx >= 0 and self.preset_combo.currentData() != 'custom':
            self.preset_combo.blockSignals(True)
            self.preset_combo.setCurrentIndex(idx)
            self.preset_combo.blockSignals(False)
            self.preset_note_lbl.setText('Manual filter selection.')
            self.preset_note_lbl.setStyleSheet('')
        self._update_counts()

    def _apply_git_autodetect(self):
        idx = self.preset_combo.findData('auto')
        if idx >= 0 and self.preset_combo.currentData() != 'auto':
            self.preset_combo.blockSignals(True)
            self.preset_combo.setCurrentIndex(idx)
            self.preset_combo.blockSignals(False)
        since_ver = self.scan.version if self.scan else None
        mask, rec_full, note = sfr.detect_git_filter(since_ver)
        self.preset_note_lbl.setText(note)
        self.preset_note_lbl.setStyleSheet('color:#d07020; font-weight:bold;' if rec_full else 'color:#2a7f62;')
        self._set_mask(mask)

    def _on_preset_changed(self):
        key = self.preset_combo.currentData()
        if key == 'auto':
            self._apply_git_autodetect()
        elif key == 'custom':
            if not self.btn_adv.isChecked():
                self.btn_adv.setChecked(True)
            self.preset_note_lbl.setText('Check any category below.')
            self.preset_note_lbl.setStyleSheet('')
            self._update_counts()
        else:
            _, mask = sfr.PRESETS.get(key, ('', 0))
            self.preset_note_lbl.setText('' if key != 'keys_only' else 'Only shader keys pasted below will be rebuilt.')
            self.preset_note_lbl.setStyleSheet('')
            self._set_mask(mask)

    def _update_counts(self):
        if not self.scan:
            self.preview_lbl.setText('No older dmsh_vk.lib selected or found.')
            return
        ck = sfr.parse_custom_keys(self.custom_edit.text())
        pairs = self.ps_to_vs if self.chk_pair_vs.isChecked() else None
        reb, keep = sfr.select_rebuild_keys(self.scan, self._selected_mask(), ck, ps_to_vs=pairs)
        pct_save = 100.0 * len(keep) / max(1, self.scan.total)
        self.preview_lbl.setText(
            f'Library v-{self.scan.version} ({self.scan.total:,} shaders) -> '
            f'Rebuild: {len(reb):,} | Skip/Migrate: {len(keep):,} (Saves {pct_save:.1f}% translation time)'
        )

    def _update_mode_ui(self):
        fast = self.rb_fast.isChecked()
        self.old_row_widget.setVisible(fast)
        self.filter_box.setVisible(fast)
        self.full_opts.setVisible(not fast)

    def append_log(self, text: str, is_red: bool):
        if is_red:
            fmt = QtGui.QTextCharFormat()
            fmt.setForeground(QtGui.QColor('#F87171'))
            cur = self.log_box.textCursor()
            cur.movePosition(QtGui.QTextCursor.MoveOperation.End)
            cur.insertText(text + '\n', fmt)
        elif text.startswith('$ '):
            fmt = QtGui.QTextCharFormat()
            fmt.setForeground(QtGui.QColor('#60A5FA'))
            cur = self.log_box.textCursor()
            cur.movePosition(QtGui.QTextCursor.MoveOperation.End)
            cur.insertText(text + '\n', fmt)
        else:
            self.log_box.appendPlainText(text)
        sb = self.log_box.verticalScrollBar()
        sb.setValue(sb.maximum())

    def start_build(self):
        if not self.ctx:
            QtWidgets.QMessageBox.warning(self, 'Info', 'Please select a game or work folder first.')
            return
        if self.ctx.profile_error:
            QtWidgets.QMessageBox.critical(self, 'Error', self.ctx.profile_error)
            return

        mode = 'fast' if self.rb_fast.isChecked() else 'full'
        params: dict = {
            'ctx': self.ctx,
            'mode': mode,
            'threads': self.spin_j.value(),
            'sync_pack': self.chk_sync_pack.isChecked(),
            'require_all': self.chk_req_all.isChecked(),
            'use_pack_android': self.chk_pack_android.isChecked(),
            'keep_code': self.chk_keep_code.isChecked(),
            'use_ooz': self.chk_ooz.isEnabled() and self.chk_ooz.isChecked(),
            'ooz_path': self.ooz_edit.text().strip(),
        }
        if mode == 'fast':
            if not self.scan:
                QtWidgets.QMessageBox.warning(self, 'Info', 'Select an older dmsh_vk.lib first (or switch to First-Time / Full Build).')
                return
            ck = sfr.parse_custom_keys(self.custom_edit.text())
            pairs = self.ps_to_vs if self.chk_pair_vs.isChecked() else None
            reb, keep = sfr.select_rebuild_keys(self.scan, self._selected_mask(), ck, ps_to_vs=pairs)
            params['src_lib'] = self.scan.path
            params['rebuild_keys'] = reb
            params['keep_keys'] = keep

        self.log_box.clear()
        self.pbar.setValue(0)
        self.btn_start.setEnabled(False)
        self.btn_stop.setEnabled(True)

        self.worker = BuildWorker(params)
        self.worker.log_line.connect(self.append_log)
        self.worker.stage_changed.connect(self.stage_lbl.setText)
        self.worker.progress_changed.connect(self._on_progress)
        self.worker.finished_ok.connect(self._on_finished)
        self.worker.start()

    def _on_progress(self, done: int, total: int, note: str):
        self.pbar.setRange(0, max(1, total))
        self.pbar.setValue(min(done, max(1, total)))
        self.pbar.setFormat(note)

    def _on_finished(self, ok: bool, msg: str):
        self.btn_start.setEnabled(True)
        self.btn_stop.setEnabled(False)
        self.stage_lbl.setText(msg)
        if self.ctx:
            self.load_target(str(self.ctx.input_path))
        if ok:
            QtWidgets.QMessageBox.information(self, 'Done', msg)
        else:
            QtWidgets.QMessageBox.warning(self, 'Stopped / Failed', msg)

    def stop_build(self):
        if self.worker:
            self.stage_lbl.setText('Stopping build...')
            self.worker.request_stop()

    def open_pack(self):
        if not self.ctx:
            return
        d = self.ctx.pack_dir
        if WINDOWS:
            os.startfile(str(d))
        elif sys.platform == 'darwin':
            subprocess.Popen(['open', str(d)])
        else:
            subprocess.Popen(['xdg-open', str(d)])

    @staticmethod
    def _guide_html() -> str:
        return """
        <style>
          body { font-family: sans-serif; line-height: 1.45; margin: 14px; }
          h2 { margin-top: 16px; margin-bottom: 6px; }
          ul { margin-top: 4px; margin-bottom: 10px; }
          li { margin-bottom: 4px; }
          code { background: rgba(128,128,128,0.18); padding: 1px 4px; border-radius: 3px; }
          .tip { border-left: 3px solid #3a86ff; padding: 6px 10px; background: rgba(58,134,255,0.10); margin: 10px 0; }
        </style>
        <h1>Polar — Shader Rebuild Studio Guide</h1>
        <p>This tool supports both <b>first-time full shader builds</b> for new games and <b>fast incremental rebuilds</b> after shader/translator fixes, without re-translating unaffected shaders from scratch.</p>

        <h2>1. Game / Work Folder</h2>
        <ul>
          <li><b>.nup File…</b>: Pick a game <code>.nup</code> package. Recommended for a first-time build or when updating <code>pack.tsv</code>.</li>
          <li><b>Folder…</b>: Pick either a console dump folder (<code>base/</code>, <code>updates/</code>, <code>dlc/</code>) or an existing <code>&lt;titleID&gt;-work</code> folder.</li>
          <li>Once loaded, the status bar shows the target Vulkan chain version (<code>v-&lt;DMSH_VK_CHAIN&gt;</code>), whether <code>pkg/</code> is already split, and whether the target shader library already exists.</li>
        </ul>

        <h2>2. Build Mode</h2>
        <ul>
          <li><b>First-Time / Full Build</b>: Use this when building shaders for a game for the first time, or when you want a clean rebuild of every shader.
            <ul>
              <li><b>Run via pack_android.py</b>: Recommended when a <code>.nup</code> or dump folder is selected. Automatically splits shaders (if needed), rewrites NSO code, builds all shaders, copies the library to <code>android-pack/</code>, and writes <code>pack.tsv</code>.</li>
              <li><b>Keep rewritten code (--keep-code)</b>: Check this if NSO modules (<code>main.patched.bin</code>, etc.) are already rewritten and you only want to rebuild shaders and refresh <code>pack.tsv</code>.</li>
            </ul>
          </li>
          <li><b>Fast Hack Rebuild</b>: Use this when an older <code>dmsh_vk.lib</code> already exists and a fix only affects specific shader categories.
            <ul>
              <li><b>Source library</b>: Automatically populated with older <code>dmsh_vk.lib</code> files found in <code>-work</code> (or click <b>Browse .lib…</b> to pick another one).</li>
              <li>Unaffected shaders are migrated directly from the older <code>dmsh_vk.lib</code> (or compatible <code>results/*.r</code> files) into <code>v-&lt;target&gt;/results/</code>, so <code>vk_android_win.py</code> skips them and only translates the selected subset.</li>
            </ul>
          </li>
        </ul>

        <h2>3. What Needs Rebuilding? (Fast Mode)</h2>
        <p>Pick a simple preset instead of guessing individual flags:</p>
        <ul>
          <li><b>Auto-detect from recent Git changes</b>: Scans the latest repository changes and automatically selects the affected shader categories (or warns you if a Full Build is safer).</li>
          <li><b>Compute / Particle / Sort fixes</b>: Rebuilds compute shaders and writable storage/image shaders.</li>
          <li><b>Texture / Buffer / Bindless fixes</b>: Rebuilds buffer-sampler, texel-buffer, and bindless shaders.</li>
          <li><b>Geometry / Mesh / Vertex fixes</b>: Rebuilds vertex, geometry, tessellation, task, and mesh stages.</li>
          <li><b>Specific shaders from log only</b>: Rebuilds only the 16-hex shader IDs pasted in the log box.</li>
          <li><b>Show Advanced Filters</b>: Expands the 9 manual category checkboxes if you want fine-grained control.</li>
        </ul>
        <p>You can paste full <code>[RED]</code> log lines directly into <b>Paste error log or shader keys</b>; 16-hex IDs are extracted automatically. The live preview line shows <code>Rebuild: X | Skip/Migrate: Y</code> before you start.</p>

        <div class="tip">
          <b>Tip:</b> If you only edited C/host runtime files (such as <code>vka_tex.c</code> or <code>vka_draw.c</code>) and <code>DMSH_VK_CHAIN</code> did <b>not</b> change, you do <b>not</b> need to rebuild the shader pack at all—just rebuild the Android APK.
        </div>

        <h2>4. Run Options &amp; Controls</h2>
        <ul>
          <li><b>Threads (J)</b>: Number of parallel translator jobs. Use <code>2–4</code> on 2–4 core CPUs, or <code>8–12</code> on high-core-count CPUs.</li>
          <li><b>Copy to android-pack &amp; update pack.tsv</b>: Keep checked so the built <code>dmsh_vk.lib</code> and <code>inputs.tsv</code> are copied to <code>android-pack/shaders/v-.../</code> and <code>pack.tsv</code> points to the new library.</li>
          <li><b>DMSH_VK_REQUIRE_ALL=1</b>: Keep checked so the build stops if any shader fails translation or <code>spirv-val</code> validation.</li>
          <li><b>Start Build</b> / <b>Stop</b> / <b>Open Pack</b>: Start the job, cancel a running build cleanly, or open the output <code>android-pack/</code> folder.</li>
        </ul>
        """


def main():
    app = QtWidgets.QApplication(sys.argv)
    initial = sys.argv[1] if len(sys.argv) > 1 else None
    win = ShaderRebuildWindow(initial)
    win.show()
    sys.exit(app.exec())


if __name__ == '__main__':
    main()
