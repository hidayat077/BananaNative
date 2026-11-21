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

    def run(self):
        p = self.params
        ctx: sfr.GameContext = p['ctx']
        mode: str = p['mode']
        threads: int = p['threads']
        try:
            if mode == 'full':
                self.stage_changed.emit('Running full build (Normal Mode)...')
                if p['use_pack_android'] and ctx.game_source:
                    cmd, env = sfr.build_pack_command(ctx, threads, keep_code=p['keep_code'])
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
                cmd_sp, env_sp = sfr.build_pack_command(ctx, threads)
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
                else:
                    sfr.sync_to_pack_manual(ctx.work_dir, ctx.pack_dir, ctx.title_id, ctx.target_vk)

            self.finished_ok.emit(True, f'Fast rebuild complete! Output: {ctx.pack_dir / "shaders" / f"v-{ctx.target_vk}"}')
        except Exception as e:
            self.log_line.emit(f'[RED] {e}', True)
            self.finished_ok.emit(False, str(e))


class ShaderRebuildWindow(QtWidgets.QMainWindow):
    def __init__(self, initial: str | None = None):
        super().__init__()
        self.setWindowTitle('Banana Native — Shader Rebuild Studio')
        self.resize(980, 760)
        self.ctx: sfr.GameContext | None = None
        self.scan: sfr.LibraryScan | None = None
        self.worker: BuildWorker | None = None

        central = QtWidgets.QWidget()
        self.setCentralWidget(central)
        outer = QtWidgets.QVBoxLayout(central)
        self.tabs = QtWidgets.QTabWidget()
        outer.addWidget(self.tabs)

        builder_tab = QtWidgets.QWidget()
        root = QtWidgets.QVBoxLayout(builder_tab)
        root.setSpacing(10)
        self.tabs.addTab(builder_tab, 'Builder')

        # 1. Source Picker
        src_box = QtWidgets.QGroupBox('1. Game / Work Folder')
        src_lay = QtWidgets.QGridLayout(src_box)
        self.path_edit = QtWidgets.QLineEdit()
        self.path_edit.setPlaceholderText('Select a .nup file, dump folder (base/, updates/), or <titleID>-work folder...')
        btn_nup = QtWidgets.QPushButton('.nup File…')
        btn_dir = QtWidgets.QPushButton('Folder…')
        btn_load = QtWidgets.QPushButton('Load')
        btn_nup.clicked.connect(self.pick_nup)
        btn_dir.clicked.connect(self.pick_dir)
        btn_load.clicked.connect(lambda: self.load_target(self.path_edit.text().strip()))
        src_lay.addWidget(self.path_edit, 0, 0)
        src_lay.addWidget(btn_nup, 0, 1)
        src_lay.addWidget(btn_dir, 0, 2)
        src_lay.addWidget(btn_load, 0, 3)

        self.info_lbl = QtWidgets.QLabel('No game selected.')
        self.info_lbl.setWordWrap(True)
        src_lay.addWidget(self.info_lbl, 1, 0, 1, 4)
        root.addWidget(src_box)

        # 2. Mode + Old Source
        mode_box = QtWidgets.QGroupBox('2. Build Mode')
        mode_lay = QtWidgets.QVBoxLayout(mode_box)
        top_mode = QtWidgets.QHBoxLayout()
        self.rb_fast = QtWidgets.QRadioButton('Fast Hack Rebuild (skip unaffected shaders from an older library)')
        self.rb_full = QtWidgets.QRadioButton('First-Time / Full Build (split & translate all shaders)')
        self.rb_fast.toggled.connect(self._update_mode_ui)
        top_mode.addWidget(self.rb_fast)
        top_mode.addWidget(self.rb_full)
        mode_lay.addLayout(top_mode)

        old_row = QtWidgets.QHBoxLayout()
        old_row.addWidget(QtWidgets.QLabel('Source library:'))
        self.lib_combo = QtWidgets.QComboBox()
        self.lib_combo.currentIndexChanged.connect(self._on_lib_selected)
        old_row.addWidget(self.lib_combo, 1)
        btn_browse_lib = QtWidgets.QPushButton('Browse .lib…')
        btn_browse_lib.clicked.connect(self.pick_old_lib)
        old_row.addWidget(btn_browse_lib)
        mode_lay.addLayout(old_row)

        self.full_opts = QtWidgets.QWidget()
        fo_lay = QtWidgets.QHBoxLayout(self.full_opts)
        fo_lay.setContentsMargins(0, 0, 0, 0)
        self.chk_pack_android = QtWidgets.QCheckBox('Run via pack_android.py (split + code rewrite + shaders + pack.tsv)')
        self.chk_pack_android.setChecked(True)
        self.chk_pack_android.setToolTip('Recommended for first-time setup or full pack refresh from a .nup or dump folder.')
        self.chk_keep_code = QtWidgets.QCheckBox('Keep rewritten code (--keep-code)')
        self.chk_keep_code.setToolTip('Skip NSO code rewriting if main.patched.bin is already up to date.')
        fo_lay.addWidget(self.chk_pack_android)
        fo_lay.addWidget(self.chk_keep_code)
        fo_lay.addStretch()
        mode_lay.addWidget(self.full_opts)
        root.addWidget(mode_box)

        # 3. Filter Checklist for Fast Mode
        self.filter_box = QtWidgets.QGroupBox('3. Select Shader Categories to Rebuild (Fast Mode)')
        flt_lay = QtWidgets.QVBoxLayout(self.filter_box)
        grid = QtWidgets.QGridLayout()
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
            cb.toggled.connect(self._update_counts)
            grid.addWidget(cb, idx // 3, idx % 3)
        flt_lay.addLayout(grid)

        custom_row = QtWidgets.QHBoxLayout()
        custom_row.addWidget(QtWidgets.QLabel('Custom shader keys (16-hex IDs from log):'))
        self.custom_edit = QtWidgets.QLineEdit()
        self.custom_edit.setPlaceholderText('e.g. c254eaa40e0c4f1e 5854663e6002c250 ...')
        self.custom_edit.textChanged.connect(self._update_counts)
        custom_row.addWidget(self.custom_edit, 1)
        flt_lay.addLayout(custom_row)

        self.preview_lbl = QtWidgets.QLabel('Select or scan an older dmsh_vk.lib to preview shader counts.')
        self.preview_lbl.setStyleSheet('font-weight: bold;')
        flt_lay.addWidget(self.preview_lbl)
        root.addWidget(self.filter_box)

        # 4. Run options + Controls
        ctrl_row = QtWidgets.QHBoxLayout()
        ctrl_row.addWidget(QtWidgets.QLabel('Threads (J):'))
        self.spin_j = QtWidgets.QSpinBox()
        self.spin_j.setRange(1, max(16, (os.cpu_count() or 8) * 2))
        self.spin_j.setValue(min(4, os.cpu_count() or 4))
        ctrl_row.addWidget(self.spin_j)

        self.chk_sync_pack = QtWidgets.QCheckBox('Copy to android-pack & update pack.tsv')
        self.chk_sync_pack.setChecked(True)
        self.chk_req_all = QtWidgets.QCheckBox('DMSH_VK_REQUIRE_ALL=1')
        self.chk_req_all.setChecked(True)
        self.chk_req_all.setToolTip('Fail the build if any shader fails to translate or validate.')
        ctrl_row.addWidget(self.chk_sync_pack)
        ctrl_row.addWidget(self.chk_req_all)
        ctrl_row.addStretch()

        self.btn_start = QtWidgets.QPushButton('Start Build')
        self.btn_start.setMinimumWidth(130)
        self.btn_start.clicked.connect(self.start_build)
        self.btn_stop = QtWidgets.QPushButton('Stop')
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
        root.addLayout(ctrl_row)

        self.stage_lbl = QtWidgets.QLabel('Ready.')
        root.addWidget(self.stage_lbl)
        self.pbar = QtWidgets.QProgressBar()
        self.pbar.setRange(0, 100)
        root.addWidget(self.pbar)

        self.log_box = QtWidgets.QPlainTextEdit()
        self.log_box.setReadOnly(True)
        font = QtGui.QFont('Monospace', 9)
        font.setStyleHint(QtGui.QFont.StyleHint.TypeWriter)
        self.log_box.setFont(font)
        root.addWidget(self.log_box, 1)

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
        self._update_counts()

    def _selected_mask(self) -> int:
        mask = 0
        for cb, flag in self.filter_checks:
            if cb.isChecked():
                mask |= flag
        return mask

    def _update_counts(self):
        if not self.scan:
            self.preview_lbl.setText('No older dmsh_vk.lib selected or found.')
            return
        ck = sfr.parse_custom_keys(self.custom_edit.text())
        reb, keep = sfr.select_rebuild_keys(self.scan, self._selected_mask(), ck)
        pct_save = 100.0 * len(keep) / max(1, self.scan.total)
        self.preview_lbl.setText(
            f'Library v-{self.scan.version} ({self.scan.total:,} shaders) -> '
            f'Rebuild: {len(reb):,} | Skip/Migrate: {len(keep):,} (Saves {pct_save:.1f}% translation time)'
        )

    def _update_mode_ui(self):
        fast = self.rb_fast.isChecked()
        self.lib_combo.setEnabled(fast)
        self.filter_box.setEnabled(fast)
        self.full_opts.setEnabled(not fast)

    def append_log(self, text: str, is_red: bool):
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
        }
        if mode == 'fast':
            if not self.scan:
                QtWidgets.QMessageBox.warning(self, 'Info', 'Select an older dmsh_vk.lib first (or switch to First-Time / Full Build).')
                return
            ck = sfr.parse_custom_keys(self.custom_edit.text())
            reb, keep = sfr.select_rebuild_keys(self.scan, self._selected_mask(), ck)
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
        <h1>Banana Native — Shader Rebuild Studio Guide</h1>
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

        <h2>3. Select Shader Categories to Rebuild (Fast Mode)</h2>
        <p>Check only the categories affected by your fix (you can combine multiple checkboxes):</p>
        <ul>
          <li><b>Compute shaders (cs)</b>: Rebuilds all compute shaders (e.g. after <code>AstOptimizer</code>, barrier, or subgroup fixes).</li>
          <li><b>Buffer + COMBINED_IMAGE_SAMPLER</b>: Rebuilds shaders reading buffer textures through combined sampler slots (e.g. <code>buffer_textures_split</code>).</li>
          <li><b>Storage / Image Write (BF_RW)</b>: Rebuilds shaders that write to storage buffers or storage images.</li>
          <li><b>Texel Buffer (tbuf)</b>: Rebuilds shaders using <code>UNIFORM_TEXEL_BUFFER</code> or <code>STORAGE_TEXEL_BUFFER</code>.</li>
          <li><b>Bindless / Dynamic Slots (tex_dyn/pool)</b>: Rebuilds shaders using bindless texture/sampler pools or runtime-indexed slots.</li>
          <li><b>Storage Buffer / GMEM map</b>: Rebuilds shaders reading raw global memory pools or storage buffers.</li>
          <li><b>All Vertex (vs)</b> / <b>All Pixel (ps)</b> / <b>Geometry / Tess / Mesh</b>: Rebuilds every shader in those stages.</li>
          <li><b>Custom shader keys</b>: Paste 16-hex-digit shader IDs from logs (e.g. <code>c254eaa40e0c4f1e</code>, separated by spaces or newlines) to rebuild specific shaders only.</li>
        </ul>
        <p>The live preview line shows the exact count before starting: <code>Rebuild: X | Skip/Migrate: Y (Saves Z% translation time)</code>.</p>

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
