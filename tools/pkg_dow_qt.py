#!/usr/bin/env python3
"""PKG_DoW HyperCore native launcher (PyQt6).

Native UX for the modified KytyPS5 core and the bundled PS4 core slot. The launcher keeps
stdout/stderr attached to the terminal, mirrors engine output into the Logs page, manages a
local game library, performs Vulkan preflight checks and exposes the Kyty keyboard/mouse ->
DualSense keymap plus automatic SDL/XInput gamepad discovery.
"""
from __future__ import annotations

import ctypes
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

try:
    from PyQt6.QtCore import Qt, QProcess, QTimer, pyqtSignal
    from PyQt6.QtGui import QKeyEvent, QMouseEvent
    from PyQt6.QtWidgets import (
        QApplication, QCheckBox, QComboBox, QDialog, QFileDialog, QFormLayout, QFrame,
        QGridLayout, QHBoxLayout, QHeaderView, QLabel, QLineEdit, QListWidget, QListWidgetItem,
        QMainWindow, QMessageBox, QPushButton, QSlider, QSpinBox, QStackedWidget, QTableWidget,
        QTableWidgetItem, QTextEdit, QVBoxLayout, QWidget,
    )
except Exception as exc:  # handled by 1.bat too, but leave a useful direct-run error
    print(f"[FATAL] PyQt6 nao esta disponivel: {exc}", flush=True)
    print("Execute 1.bat para instalar o runtime local automaticamente.", flush=True)
    raise

import pkg_dow_server as core
import engine_doctor

DATA = ROOT / "userdata"
LOGS = ROOT / "logs"
LOGS.mkdir(parents=True, exist_ok=True)

APP_VERSION = "2.5.0-pkg"
EARLY_CRASH_SECONDS = 20

# These IDs intentionally match src/graphics/presentation/window/hostInput.cpp.
CONTROL_ROWS = [
    ("Up", "D-pad ↑", "Up"), ("Down", "D-pad ↓", "Down"),
    ("Left", "D-pad ←", "Left"), ("Right", "D-pad →", "Right"),
    ("LeftStickUp", "Analógico esquerdo ↑", "W"),
    ("LeftStickDown", "Analógico esquerdo ↓", "S"),
    ("LeftStickLeft", "Analógico esquerdo ←", "A"),
    ("LeftStickRight", "Analógico esquerdo →", "D"),
    ("RightStickUp", "Analógico direito ↑", "T"),
    ("RightStickDown", "Analógico direito ↓", "G"),
    ("RightStickLeft", "Analógico direito ←", "F"),
    ("RightStickRight", "Analógico direito →", "H"),
    ("Triangle", "△ Triângulo", "I"), ("Circle", "○ Círculo", "L"),
    ("Cross", "× Cruz", "J"), ("Square", "□ Quadrado", "K"),
    ("L1", "L1", "Q"), ("R1", "R1", "E"), ("L2", "L2", ""), ("R2", "R2", ""),
    ("L3", "L3", "Left Shift"), ("R3", "R3", "Left Ctrl"),
    ("Options", "Options", "Return"),
    ("TouchPad", "Touchpad esquerdo", "Backspace"),
    ("TouchPadRight", "Touchpad direito", "Tab"),
]

DEFAULT_MAPPING = {cid: default for cid, _label, default in CONTROL_ROWS if default}


def log_console(text: str) -> None:
    stamp = time.strftime("%H:%M:%S")
    for line in str(text).rstrip("\n").splitlines() or [""]:
        print(f"[{stamp}] {line}", flush=True)


def human_bytes(value: int) -> str:
    n = float(max(0, int(value)))
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if n < 1024.0 or unit == "TiB":
            return f"{n:.1f} {unit}"
        n /= 1024.0
    return f"{n:.1f} TiB"


def integrated_engines() -> dict[str, str]:
    """Only product-managed locations. No manual path setting in the UI."""
    ps5_candidates = [
        # Prefer freshly built core. Packaged copy may be stale after a source rebuild.
        ROOT / "_Build/windows/kyty_emulator.exe",
        ROOT / "_Build/windows/install/kyty_emulator.exe",
        ROOT / "_Build/windows/install/bin/kyty_emulator.exe",
        ROOT / "engines/ps5/kyty_emulator.exe",
        ROOT / "bin/kyty_emulator.exe",
        ROOT / "kyty_emulator.exe",
    ]
    ps4_candidates = [
        ROOT / "engines/ps4/core-0.18.0/shadPS4.exe",
        ROOT / "engines/ps4/core-0.18.0/shadps4.exe",
        ROOT / "engines/ps4/shadPS4.exe",
        ROOT / "engines/ps4/shadps4.exe",
        ROOT / "engines/shadps4/shadPS4.exe",
        ROOT / "engines/shadps4/shadps4.exe",
        ROOT / "shadPS4.exe", ROOT / "shadps4.exe",
    ]
    sharpemu_candidates = [
        ROOT / "engines/ps5/sharpemu/SharpEmu.exe",
        ROOT / "engines/ps5/sharpemu/sharpemu.exe",
        ROOT / "engines/ps5/SharpEmu.exe",
        ROOT / "engines/sharpemu/SharpEmu.exe",
        ROOT / "SharpEmu/SharpEmu.exe",
        ROOT / "sharpemu/SharpEmu.exe",
    ]
    def first(paths: list[Path]) -> str:
        for p in paths:
            if p.is_file():
                return str(p.resolve())
        return ""
    pkg_installer_candidates = [
        ROOT / "tools/pkgtool/PkgTool.Core.exe",
    ]
    return {"kyty": first(ps5_candidates), "sharpemu": first(sharpemu_candidates),
            "shadps4": first(ps4_candidates), "pkg_installer": first(pkg_installer_candidates)}


def load_settings() -> dict[str, Any]:
    cfg = core.settings()
    cfg.setdefault("keymap", [])
    cfg.setdefault("mouse_sensitivity", 1.0)
    cfg.setdefault("controller_deadzone", 10)
    cfg.setdefault("auto_safe_retry", True)
    # Deprecated external paths are deliberately ignored.
    cfg["kyty_path"] = ""
    cfg["shadps4_path"] = ""
    return cfg


def save_settings(cfg: dict[str, Any]) -> None:
    cfg = dict(cfg)
    cfg["kyty_path"] = ""
    cfg["shadps4_path"] = ""
    core.save_json(core.SETTINGS_FILE, cfg)


def parse_keymap(entries: list[str]) -> dict[str, str]:
    out: dict[str, str] = {}
    for entry in entries:
        if entry.startswith("MouseSensitivity="):
            continue
        if "=" in entry:
            key, value = entry.split("=", 1)
            if key and value:
                out[key] = value
    return out


def serialize_keymap(mapping: dict[str, str], sensitivity: float) -> list[str]:
    result = [f"{cid}={mapping[cid]}" for cid, _label, _default in CONTROL_ROWS if mapping.get(cid)]
    if abs(float(sensitivity) - 1.0) > 1e-6:
        result.append(f"MouseSensitivity={float(sensitivity):.1f}")
    return result


def detect_xinput() -> list[str]:
    if os.name != "nt":
        return []
    dll = None
    for name in ("xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"):
        try:
            dll = ctypes.WinDLL(name)
            break
        except OSError:
            continue
    if dll is None:
        return []

    class XINPUT_GAMEPAD(ctypes.Structure):
        _fields_ = [("wButtons", ctypes.c_ushort), ("bLeftTrigger", ctypes.c_ubyte),
                    ("bRightTrigger", ctypes.c_ubyte), ("sThumbLX", ctypes.c_short),
                    ("sThumbLY", ctypes.c_short), ("sThumbRX", ctypes.c_short),
                    ("sThumbRY", ctypes.c_short)]
    class XINPUT_STATE(ctypes.Structure):
        _fields_ = [("dwPacketNumber", ctypes.c_ulong), ("Gamepad", XINPUT_GAMEPAD)]
    fn = dll.XInputGetState
    fn.argtypes = [ctypes.c_ulong, ctypes.POINTER(XINPUT_STATE)]
    fn.restype = ctypes.c_ulong
    found: list[str] = []
    for idx in range(4):
        state = XINPUT_STATE()
        if int(fn(idx, ctypes.byref(state))) == 0:
            found.append(f"XInput #{idx + 1} (Xbox/compatível)")
    return found


def detect_generic_windows() -> list[str]:
    if os.name != "nt":
        return []
    script = r"""
$ErrorActionPreference='SilentlyContinue'
Get-CimInstance Win32_PnPEntity | Where-Object {
 $_.Status -eq 'OK' -and ($_.Name -match 'controller|gamepad|xbox|dualsense|dualshock|wireless controller')
} | Select-Object -ExpandProperty Name -Unique
"""
    try:
        p = subprocess.run(["powershell.exe", "-NoLogo", "-NoProfile", "-Command", script],
                           text=True, capture_output=True, timeout=8, check=False)
        return [x.strip() for x in p.stdout.splitlines() if x.strip()]
    except Exception:
        return []


class BindingCapture(QDialog):
    captured = pyqtSignal(str)

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.value = ""
        self.setWindowTitle("Mapear entrada")
        self.setModal(True)
        self.resize(430, 150)
        layout = QVBoxLayout(self)
        self.label = QLabel("Pressione uma tecla ou botão do mouse.\nEsc cancela. F1/F7/F11 são reservadas.")
        self.label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(self.label)

    def keyPressEvent(self, event: QKeyEvent) -> None:  # noqa: N802
        if event.isAutoRepeat():
            return
        k = event.key()
        if k == Qt.Key.Key_Escape:
            self.reject(); return
        if k in (Qt.Key.Key_F1, Qt.Key.Key_F7, Qt.Key.Key_F11, Qt.Key.Key_Space):
            self.label.setText("Essa tecla é reservada pelo core."); return
        special = {
            Qt.Key.Key_Return: "Return", Qt.Key.Key_Enter: "Return",
            Qt.Key.Key_Backspace: "Backspace", Qt.Key.Key_Tab: "Tab",
            Qt.Key.Key_Shift: "Left Shift", Qt.Key.Key_Control: "Left Ctrl",
            Qt.Key.Key_Alt: "Left Alt", Qt.Key.Key_Meta: "Left GUI",
            Qt.Key.Key_Up: "Up", Qt.Key.Key_Down: "Down",
            Qt.Key.Key_Left: "Left", Qt.Key.Key_Right: "Right",
            Qt.Key.Key_Delete: "Delete", Qt.Key.Key_Insert: "Insert",
            Qt.Key.Key_Home: "Home", Qt.Key.Key_End: "End",
            Qt.Key.Key_PageUp: "PageUp", Qt.Key.Key_PageDown: "PageDown",
        }
        if k in special:
            self.value = special[k]
        elif Qt.Key.Key_F1 <= k <= Qt.Key.Key_F24:
            self.value = f"F{k - Qt.Key.Key_F1 + 1}"
        elif Qt.Key.Key_A <= k <= Qt.Key.Key_Z:
            self.value = chr(k)
        elif Qt.Key.Key_0 <= k <= Qt.Key.Key_9:
            self.value = chr(k)
        else:
            text = event.text().strip()
            if len(text) == 1 and text.isprintable():
                self.value = text.upper()
        if self.value:
            self.accept()
        else:
            self.label.setText("Tecla não suportada pelo mapeador SDL atual.")

    def mousePressEvent(self, event: QMouseEvent) -> None:  # noqa: N802
        names = {
            Qt.MouseButton.LeftButton: "Mouse:Left",
            Qt.MouseButton.RightButton: "Mouse:Right",
            Qt.MouseButton.MiddleButton: "Mouse:Middle",
            Qt.MouseButton.BackButton: "Mouse:X1",
            Qt.MouseButton.ForwardButton: "Mouse:X2",
        }
        value = names.get(event.button(), "")
        if value:
            self.value = value
            self.accept()


class HyperCoreWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.cfg = load_settings()
        self.engines = integrated_engines()
        self.vulkan = core.vulkan_probe(force=True)
        self.process: QProcess | None = None
        self.process_started = 0.0
        self.current_game: dict[str, Any] | None = None
        self.current_safe_retry = False
        self.current_log_file = None
        self.current_log_path: Path | None = None
        self.log_buffer: list[str] = []
        self.core_log_buffer: list[str] = []
        self.recovery_pending = False
        self.launch_serial = 0
        self.launcher_hidden_for_game = False
        self.launcher_window_state = Qt.WindowState.WindowNoState
        self.engine_runtime_cache: dict[str, dict[str, Any]] = {}
        self.mapping = parse_keymap(list(self.cfg.get("keymap") or [])) or dict(DEFAULT_MAPPING)

        self.setWindowTitle(f"PKG_DoW HyperCore {APP_VERSION}")
        self.resize(1460, 900)
        self.setMinimumSize(1180, 720)
        self._build_ui()
        self._apply_style()
        self.refresh_all()
        self._log_banner()

    def _log_banner(self) -> None:
        self.log(f"PKG_DoW HyperCore {APP_VERSION} iniciado")
        self.log(f"Raiz: {ROOT}")
        self.log(f"Python: {sys.version.split()[0]} | Qt: PyQt6")
        self.log(f"Core PS5: {self.engines['kyty'] or 'AUSENTE'}")
        self.log(f"SharpEmu: {self.engines['sharpemu'] or 'AUSENTE (fallback opcional)'}")
        self.log(f"Core PS4: {self.engines['shadps4'] or 'AUSENTE'}")
        self.log(f"Instalador PKG PS4: {self.engines['pkg_installer'] or 'AUSENTE'}")
        if self.vulkan.get("available"):
            for gpu in self.vulkan.get("gpus", []):
                self.log(f"Vulkan GPU {gpu.get('index')}: {gpu.get('name')} | {gpu.get('api_version')} | {human_bytes(gpu.get('vram', 0))}")
        else:
            self.log(f"Vulkan indisponível: {self.vulkan.get('error', 'erro desconhecido')}")

    def _build_ui(self) -> None:
        root = QWidget(); self.setCentralWidget(root)
        outer = QHBoxLayout(root); outer.setContentsMargins(0, 0, 0, 0); outer.setSpacing(0)

        sidebar = QFrame(); sidebar.setObjectName("sidebar"); sidebar.setFixedWidth(230)
        sl = QVBoxLayout(sidebar); sl.setContentsMargins(20, 24, 20, 24); sl.setSpacing(10)
        logo = QLabel("PKG_<span style='color:#8b5cf6'>DoW</span>")
        logo.setTextFormat(Qt.TextFormat.RichText); logo.setObjectName("logo"); sl.addWidget(logo)
        sub = QLabel("HyperCore • PS4 / PS5"); sub.setObjectName("muted"); sl.addWidget(sub)
        sl.addSpacing(18)
        self.nav = QListWidget(); self.nav.setObjectName("nav")
        for text in ("⌂  Início", "▦  Biblioteca", "🎮  Controles", "⚙  Configurações", "◉  Sistema", "▤  Logs"):
            self.nav.addItem(QListWidgetItem(text))
        self.nav.setCurrentRow(0); sl.addWidget(self.nav, 1)
        self.status_dot = QLabel("● pronto"); self.status_dot.setObjectName("okText"); sl.addWidget(self.status_dot)
        outer.addWidget(sidebar)

        self.stack = QStackedWidget(); outer.addWidget(self.stack, 1)
        self.page_home = self._home_page(); self.stack.addWidget(self.page_home)
        self.page_library = self._library_page(); self.stack.addWidget(self.page_library)
        self.page_controls = self._controls_page(); self.stack.addWidget(self.page_controls)
        self.page_settings = self._settings_page(); self.stack.addWidget(self.page_settings)
        self.page_system = self._system_page(); self.stack.addWidget(self.page_system)
        self.page_logs = self._logs_page(); self.stack.addWidget(self.page_logs)
        self.nav.currentRowChanged.connect(self.stack.setCurrentIndex)

    def page_shell(self, title: str, subtitle: str) -> tuple[QWidget, QVBoxLayout]:
        page = QWidget(); layout = QVBoxLayout(page); layout.setContentsMargins(32, 28, 32, 28); layout.setSpacing(18)
        h = QLabel(title); h.setObjectName("pageTitle"); layout.addWidget(h)
        p = QLabel(subtitle); p.setWordWrap(True); p.setObjectName("muted"); layout.addWidget(p)
        return page, layout

    def card(self) -> QFrame:
        f = QFrame(); f.setObjectName("card"); return f

    def _home_page(self) -> QWidget:
        page, l = self.page_shell("Bem-vindo de volta", "Core integrado, Vulkan, biblioteca, controles e logs numa aplicação nativa. Sem navegador fazendo cosplay de launcher.")
        hero = self.card(); hl = QHBoxLayout(hero); hl.setContentsMargins(26, 24, 26, 24)
        left = QVBoxLayout(); self.home_title = QLabel("Pronto para jogar?"); self.home_title.setObjectName("heroTitle"); left.addWidget(self.home_title)
        self.home_summary = QLabel(""); self.home_summary.setObjectName("muted"); self.home_summary.setWordWrap(True); left.addWidget(self.home_summary)
        buttons = QHBoxLayout(); imp = QPushButton("Importar jogo"); imp.clicked.connect(self.import_game); buttons.addWidget(imp)
        folder = QPushButton("Importar pasta"); folder.setProperty("secondary", True); folder.clicked.connect(self.import_folder); buttons.addWidget(folder)
        buttons.addStretch(); left.addLayout(buttons); hl.addLayout(left, 3)
        self.home_gpu = QLabel("GPU\n-"); self.home_gpu.setObjectName("metricBig"); hl.addWidget(self.home_gpu, 2)
        l.addWidget(hero)

        grid = QGridLayout(); grid.setSpacing(16)
        self.metric_games = self.metric_card("Biblioteca", "0 jogos")
        self.metric_ps5 = self.metric_card("Core PS5", "ausente")
        self.metric_ps4 = self.metric_card("Core PS4", "ausente")
        self.metric_vulkan = self.metric_card("Vulkan", "-")
        grid.addWidget(self.metric_games, 0, 0); grid.addWidget(self.metric_ps5, 0, 1)
        grid.addWidget(self.metric_ps4, 0, 2); grid.addWidget(self.metric_vulkan, 0, 3)
        l.addLayout(grid)
        self.recent_table = self.make_game_table(); l.addWidget(self.recent_table, 1)
        return page

    def metric_card(self, title: str, value: str) -> QFrame:
        c = self.card(); lay = QVBoxLayout(c); lay.setContentsMargins(18, 16, 18, 16)
        t = QLabel(title); t.setObjectName("muted"); v = QLabel(value); v.setObjectName("metricValue"); v.setProperty("metricValue", True)
        lay.addWidget(t); lay.addWidget(v); c._value_label = v  # type: ignore[attr-defined]
        return c

    def make_game_table(self) -> QTableWidget:
        t = QTableWidget(0, 6); t.setHorizontalHeaderLabels(["Jogo", "ID", "Plataforma", "Status", "Última execução", ""])
        t.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows); t.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        t.verticalHeader().setVisible(False); t.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
        for col in (1, 2, 3, 4, 5): t.horizontalHeader().setSectionResizeMode(col, QHeaderView.ResizeMode.ResizeToContents)
        return t

    def _library_page(self) -> QWidget:
        page, l = self.page_shell("Biblioteca", "PKG PS4 é extraído em segundo plano dentro do PKG_DoW.")
        bar = QHBoxLayout(); b1 = QPushButton("+ Importar jogo"); b1.clicked.connect(self.import_game); bar.addWidget(b1)
        b2 = QPushButton("+ Importar pasta"); b2.setProperty("secondary", True); b2.clicked.connect(self.import_folder); bar.addWidget(b2)
        pkg = QPushButton("📦 Importar PKG"); pkg.setProperty("secondary", True); pkg.clicked.connect(self.open_pkg_installer); bar.addWidget(pkg)
        sync = QPushButton("⟳ Sincronizar PS4"); sync.setProperty("secondary", True); sync.clicked.connect(self.sync_ps4_games); bar.addWidget(sync)
        refresh = QPushButton("Atualizar"); refresh.setProperty("secondary", True); refresh.clicked.connect(self.refresh_library); bar.addWidget(refresh); bar.addStretch(); l.addLayout(bar)
        self.library_table = self.make_game_table(); l.addWidget(self.library_table, 1)
        return page

    def _controls_page(self) -> QWidget:
        page, l = self.page_shell("Controles", "Teclado e mouse são traduzidos para o pad guest no Kyty. Xbox, DualShock/DualSense e gamepads genéricos entram pelo SDL automaticamente; o painel também mostra XInput/PnP detectados.")
        top = QHBoxLayout()
        devices = self.card(); dl = QVBoxLayout(devices); dl.addWidget(QLabel("Dispositivos conectados"))
        self.devices_list = QListWidget(); dl.addWidget(self.devices_list); r = QPushButton("Reexaminar controles"); r.clicked.connect(self.refresh_controllers); dl.addWidget(r); top.addWidget(devices, 2)
        info = self.card(); il = QVBoxLayout(info); il.addWidget(QLabel("Perfil de entrada"))
        self.mouse_slider = QSlider(Qt.Orientation.Horizontal); self.mouse_slider.setRange(1, 50); self.mouse_slider.setValue(round(float(self.cfg.get("mouse_sensitivity", 1.0)) * 10)); il.addWidget(QLabel("Sensibilidade do mouse → analógico direito")); il.addWidget(self.mouse_slider)
        self.deadzone = QSpinBox(); self.deadzone.setRange(0, 40); self.deadzone.setSuffix(" %"); self.deadzone.setValue(int(self.cfg.get("controller_deadzone", 10))); il.addWidget(QLabel("Deadzone de gamepad (perfil)")); il.addWidget(self.deadzone)
        note = QLabel("F7 durante o jogo ativa/desativa mouse relativo no analógico direito do Kyty. Gamepads físicos têm prioridade sobre o host virtual."); note.setWordWrap(True); note.setObjectName("muted"); il.addWidget(note); il.addStretch(); top.addWidget(info, 1)
        l.addLayout(top)
        self.mapping_table = QTableWidget(len(CONTROL_ROWS), 3); self.mapping_table.setHorizontalHeaderLabels(["DualSense", "Entrada do PC", "Ação"]); self.mapping_table.verticalHeader().setVisible(False)
        self.mapping_table.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch); self.mapping_table.horizontalHeader().setSectionResizeMode(1, QHeaderView.ResizeMode.Stretch); self.mapping_table.horizontalHeader().setSectionResizeMode(2, QHeaderView.ResizeMode.ResizeToContents)
        for row, (cid, label, _default) in enumerate(CONTROL_ROWS):
            self.mapping_table.setItem(row, 0, QTableWidgetItem(label)); self.mapping_table.item(row, 0).setData(Qt.ItemDataRole.UserRole, cid)
            self.mapping_table.setItem(row, 1, QTableWidgetItem(self.mapping.get(cid, "—")))
            btn = QPushButton("Mapear"); btn.setProperty("secondary", True); btn.clicked.connect(lambda _=False, rr=row: self.capture_binding(rr)); self.mapping_table.setCellWidget(row, 2, btn)
        l.addWidget(self.mapping_table, 1)
        actions = QHBoxLayout(); save = QPushButton("Salvar controles"); save.clicked.connect(self.save_controls); actions.addWidget(save)
        defaults = QPushButton("Restaurar padrão"); defaults.setProperty("secondary", True); defaults.clicked.connect(self.restore_controls); actions.addWidget(defaults); actions.addStretch(); l.addLayout(actions)
        return page

    def _settings_page(self) -> QWidget:
        page, l = self.page_shell("Configurações", "As engines são componentes do produto. Não há campo para caçar EXE manualmente no disco.")
        engines = self.card(); el = QFormLayout(engines)
        self.ps5_path = QLineEdit(); self.ps5_path.setReadOnly(True); self.sharpemu_path = QLineEdit(); self.sharpemu_path.setReadOnly(True); self.ps4_path = QLineEdit(); self.ps4_path.setReadOnly(True); self.pkg_installer_path = QLineEdit(); self.pkg_installer_path.setReadOnly(True)
        el.addRow("KytyPS5 / PS5", self.ps5_path); el.addRow("SharpEmu / PS5 fallback", self.sharpemu_path); el.addRow("Core PS4", self.ps4_path); el.addRow("Extrator PKG interno", self.pkg_installer_path); l.addWidget(engines)
        perf = self.card(); form = QFormLayout(perf)
        self.profile = QComboBox(); self.profile.addItems(["stable", "balanced", "turbo"]); self.profile.setCurrentText(str(self.cfg.get("performance_profile", "balanced"))); form.addRow("Perfil", self.profile)
        self.resolution = QComboBox(); self.resolution.addItems(["960x540", "1280x720", "1600x900", "1920x1080"]); self.resolution.setCurrentText(str(self.cfg.get("resolution", "1280x720"))); form.addRow("Resolução", self.resolution)
        self.present = QComboBox(); self.present.addItems(["Fifo", "Mailbox", "Immediate"]); self.present.setCurrentText(str(self.cfg.get("present_mode", "Fifo"))); form.addRow("Present mode", self.present)
        self.gpu_combo = QComboBox(); self.gpu_combo.addItem("Automático", -1)
        for gpu in self.vulkan.get("gpus", []): self.gpu_combo.addItem(f"{gpu.get('index')}: {gpu.get('name')} ({human_bytes(gpu.get('vram', 0))})", int(gpu.get("index", -1)))
        target_gpu = int(self.cfg.get("gpu_index", -1)); idx = self.gpu_combo.findData(target_gpu); self.gpu_combo.setCurrentIndex(max(0, idx)); form.addRow("GPU Vulkan", self.gpu_combo)
        self.fullscreen = QCheckBox("Abrir jogos em tela cheia"); self.fullscreen.setChecked(bool(self.cfg.get("fullscreen", False))); form.addRow("", self.fullscreen)
        fullscreen_note = QLabel("F11 ou Alt+Enter alterna durante o jogo. O launcher fica oculto enquanto a engine roda e volta ao fechar."); fullscreen_note.setWordWrap(True); fullscreen_note.setObjectName("muted"); form.addRow("", fullscreen_note)
        self.safe_retry = QCheckBox("Retry automático em modo seguro após crash muito cedo"); self.safe_retry.setChecked(bool(self.cfg.get("auto_safe_retry", True))); form.addRow("", self.safe_retry)
        save = QPushButton("Salvar configuração"); save.clicked.connect(self.save_config); form.addRow("", save); l.addWidget(perf); l.addStretch()
        return page

    def _system_page(self) -> QWidget:
        page, l = self.page_shell("Sistema", "Pré-flight real do host. Uma GPU não melhora por receber um adesivo escrito ULTRA BOOST, infelizmente.")
        self.system_text = QTextEdit(); self.system_text.setReadOnly(True); l.addWidget(self.system_text, 1)
        b = QPushButton("Reexaminar Vulkan e engines"); b.clicked.connect(self.refresh_system); l.addWidget(b)
        return page

    def _logs_page(self) -> QWidget:
        page, l = self.page_shell("Logs", "Tudo que o launcher e a engine enviarem para stdout/stderr aparece aqui e também no terminal aberto pelo 1.bat.")
        self.logs_text = QTextEdit(); self.logs_text.setReadOnly(True); self.logs_text.setObjectName("terminal"); l.addWidget(self.logs_text, 1)
        bar = QHBoxLayout(); clear = QPushButton("Limpar tela"); clear.setProperty("secondary", True); clear.clicked.connect(self.logs_text.clear); bar.addWidget(clear); open_logs = QPushButton("Abrir pasta de logs"); open_logs.setProperty("secondary", True); open_logs.clicked.connect(lambda: core.open_path(LOGS)); bar.addWidget(open_logs); bar.addStretch(); l.addLayout(bar)
        return page

    def _apply_style(self) -> None:
        self.setStyleSheet(r"""
QWidget { background:#070b17; color:#eef2ff; font-family:'Segoe UI'; font-size:14px; }
#sidebar { background:#090e20; border-right:1px solid #1f2a44; }
#logo { font-size:27px; font-weight:800; }
#muted { color:#9ca7bf; }
#okText { color:#44d483; font-weight:600; }
#nav { border:0; background:transparent; outline:0; }
#nav::item { padding:13px 14px; margin:2px 0; border-radius:9px; color:#cbd3e8; }
#nav::item:selected { background:#4c1d95; color:white; }
#pageTitle { font-size:30px; font-weight:800; }
#heroTitle { font-size:26px; font-weight:800; }
#metricBig { font-size:18px; font-weight:700; padding:22px; background:#0d1630; border-radius:12px; }
#card { background:#0c1326; border:1px solid #24314d; border-radius:14px; }
QPushButton { background:#6d28d9; border:1px solid #8b5cf6; padding:10px 16px; border-radius:9px; font-weight:600; }
QPushButton:hover { background:#7c3aed; }
QPushButton[secondary="true"] { background:#141f37; border-color:#31415f; }
QLineEdit,QComboBox,QSpinBox,QListWidget,QTableWidget,QTextEdit { background:#0b1427; border:1px solid #2a3857; border-radius:8px; padding:7px; selection-background-color:#6d28d9; }
QHeaderView::section { background:#111b32; color:#bac6df; padding:8px; border:0; border-bottom:1px solid #2a3857; }
QTableWidget { gridline-color:#1d2b46; }
#terminal { background:#030712; color:#c6f6d5; font-family:'Cascadia Mono','Consolas',monospace; }
QSlider::groove:horizontal { height:6px; background:#1e2b47; border-radius:3px; }
QSlider::handle:horizontal { width:16px; margin:-5px 0; background:#8b5cf6; border-radius:8px; }
""")

    def log(self, text: str) -> None:
        text = str(text)
        log_console(text)
        self.log_buffer.append(text)
        if len(self.log_buffer) > 5000: self.log_buffer = self.log_buffer[-5000:]
        if hasattr(self, "logs_text"):
            self.logs_text.append(text.rstrip("\n"))

    def refresh_all(self) -> None:
        self.refresh_library(); self.refresh_controllers(); self.refresh_system()

    def refresh_library(self) -> None:
        games = core.library()
        def fill(table: QTableWidget, values: list[dict[str, Any]]) -> None:
            table.setRowCount(len(values))
            for row, game in enumerate(values):
                when = "—" if not game.get("last_played") else time.strftime("%d/%m %H:%M", time.localtime(int(game["last_played"])))
                vals = [game.get("title") or Path(str(game.get("path", ""))).name, game.get("title_id", ""), game.get("platform", ""), game.get("status", ""), when]
                for col, value in enumerate(vals): table.setItem(row, col, QTableWidgetItem(str(value)))
                is_pkg = str(game.get("kind", "")) == "pkg"
                btn = QPushButton("Instalar" if is_pkg else "Jogar")
                btn.clicked.connect(lambda _=False, gid=str(game.get("id")): self.activate_game(gid)); table.setCellWidget(row, 5, btn)
        fill(self.library_table, games)
        recent = sorted(games, key=lambda x: int(x.get("last_played", 0) or 0), reverse=True)[:8]; fill(self.recent_table, recent)
        self.metric_games._value_label.setText(f"{len(games)} jogos")  # type: ignore[attr-defined]
        ps5_status = "Kyty integrado" if self.engines.get("kyty") else ("SharpEmu fallback" if self.engines.get("sharpemu") else "aguardando build")
        self.metric_ps5._value_label.setText(ps5_status)  # type: ignore[attr-defined]
        self.metric_ps4._value_label.setText("integrado" if self.engines.get("shadps4") else "ausente")  # type: ignore[attr-defined]
        self.home_summary.setText(f"{len(games)} item(ns) na biblioteca. O launcher faz pré-flight antes de iniciar e captura crashes precoces.")

    def import_game(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "Importar jogo", str(ROOT), "Jogos (*.pkg *.elf *.bin);;Todos (*.*)")
        if path: self._import(path)

    def import_folder(self) -> None:
        path = QFileDialog.getExistingDirectory(self, "Importar pasta de jogo", str(ROOT))
        if path: self._import(path)

    def _import(self, path: str) -> None:
        try:
            item = core.upsert_game(core.describe_import(path)); self.log(f"[LIB] Importado: {item.get('title')} | {item.get('title_id')} | {item.get('path')}"); self.refresh_library()
        except Exception as exc:
            self.log(f"[ERRO] Importação: {exc}"); QMessageBox.critical(self, "Importação falhou", str(exc))

    def activate_game(self, game_id: str) -> None:
        try:
            _items, _idx, game = core.game_by_id(game_id)
        except Exception as exc:
            QMessageBox.critical(self, "Biblioteca", str(exc)); return
        if str(game.get("kind", "")) == "pkg":
            self.install_pkg(game)
        else:
            self.launch_game(game_id)

    def install_pkg(self, game: dict[str, Any]) -> None:
        package = Path(str(game.get("path", "")))
        try:
            result = core.start_pkg_install(str(game.get("id", "")))
            self.log(f"[PKG] Extração interna iniciada job={result['job']} pacote={package}")
            self.log("[PKG] Progresso disponível na UX principal do PKG_DoW.")
        except Exception as exc:
            self.log(f"[PKG] Falha: {exc}")

    def open_pkg_installer(self) -> None:
        self.log("[PKG] Instalador gráfico desativado. Use Importar PKG na UX principal.")

    def sync_ps4_games(self) -> None:
        try:
            games = core.sync_ps4_installs()
            self.refresh_library()
            self.log(f"[PKG] Sincronizados {len(games)} jogo(s) em {core.PS4_GAMES_DIR}")
            QMessageBox.information(self, "PS4", f"{len(games)} jogo(s) sincronizado(s).")
        except Exception as exc:
            self.log(f"[PKG] Sincronização falhou: {exc}")
            QMessageBox.critical(self, "Sincronização PS4", str(exc))

    def refresh_controllers(self) -> None:
        self.devices_list.clear(); values = detect_xinput(); generic = detect_generic_windows()
        seen: set[str] = set()
        for name in values + generic:
            key = name.lower()
            if key in seen: continue
            seen.add(key); self.devices_list.addItem("● " + name)
        if not seen:
            self.devices_list.addItem("Nenhum controle físico detectado pelo launcher")
        self.devices_list.addItem("✓ Teclado/mouse: pad virtual Kyty sempre disponível")
        self.log(f"[INPUT] {len(seen)} dispositivo(s) físico(s) detectado(s); SDL no core faz a conexão final")

    def capture_binding(self, row: int) -> None:
        dlg = BindingCapture(self)
        if dlg.exec() != QDialog.DialogCode.Accepted or not dlg.value: return
        cid = self.mapping_table.item(row, 0).data(Qt.ItemDataRole.UserRole)
        # One host input maps to one guest control to avoid phantom multi-presses.
        for k, v in list(self.mapping.items()):
            if v.lower() == dlg.value.lower(): self.mapping.pop(k, None)
        self.mapping[str(cid)] = dlg.value
        self._refresh_mapping_table()

    def _refresh_mapping_table(self) -> None:
        for row in range(self.mapping_table.rowCount()):
            cid = str(self.mapping_table.item(row, 0).data(Qt.ItemDataRole.UserRole)); self.mapping_table.item(row, 1).setText(self.mapping.get(cid, "—"))

    def restore_controls(self) -> None:
        self.mapping = dict(DEFAULT_MAPPING); self.mouse_slider.setValue(10); self.deadzone.setValue(10); self._refresh_mapping_table()

    def save_controls(self) -> None:
        self.cfg["mouse_sensitivity"] = self.mouse_slider.value() / 10.0
        self.cfg["controller_deadzone"] = self.deadzone.value()
        self.cfg["keymap"] = serialize_keymap(self.mapping, self.cfg["mouse_sensitivity"])
        save_settings(self.cfg); self.log(f"[INPUT] Perfil salvo com {len(self.cfg['keymap'])} entrada(s)")
        QMessageBox.information(self, "Controles", "Perfil salvo. O Kyty recebe o mapeamento automaticamente no próximo boot.")

    def save_config(self) -> None:
        self.cfg["performance_profile"] = self.profile.currentText(); self.cfg["resolution"] = self.resolution.currentText(); self.cfg["present_mode"] = self.present.currentText(); self.cfg["gpu_index"] = int(self.gpu_combo.currentData()); self.cfg["fullscreen"] = self.fullscreen.isChecked(); self.cfg["auto_safe_retry"] = self.safe_retry.isChecked(); save_settings(self.cfg); self.log("[CFG] Configuração salva")

    def refresh_system(self) -> None:
        self.engines = integrated_engines(); self.vulkan = core.vulkan_probe(force=True)
        if hasattr(self, "ps5_path"):
            self.ps5_path.setText(self.engines["kyty"] or str(ROOT / "engines/ps5/kyty_emulator.exe") + "  [não construído]")
            self.sharpemu_path.setText(self.engines["sharpemu"] or str(ROOT / "engines/ps5/sharpemu/SharpEmu.exe") + "  [opcional]")
            self.ps4_path.setText(self.engines["shadps4"] or str(ROOT / "engines/ps4/shadPS4.exe") + "  [ausente]")
            self.pkg_installer_path.setText(self.engines["pkg_installer"] or str(ROOT / "tools/pkgtool/PkgTool.Core.exe") + "  [ausente]")
        gpu_text = "Nenhuma GPU Vulkan"; loader = self.vulkan.get("loader_version", "-")
        if self.vulkan.get("gpus"):
            g = self.vulkan["gpus"][0]; gpu_text = f"{g.get('name')}\n{g.get('architecture')} • {human_bytes(g.get('vram', 0))}"
        self.home_gpu.setText("GPU Vulkan\n" + gpu_text)
        self.metric_vulkan._value_label.setText(f"API {loader}" if self.vulkan.get("available") else "indisponível")  # type: ignore[attr-defined]
        lines = [f"Vulkan loader: {loader}", f"Core PS5: {self.engines['kyty'] or 'não construído'}", f"SharpEmu fallback: {self.engines['sharpemu'] or 'opcional ausente'}", f"Core PS4: {self.engines['shadps4'] or 'ausente'}", f"Extrator PKG interno: {self.engines['pkg_installer'] or 'ausente'}", f"Pasta de jogos PS4: {core.PS4_GAMES_DIR}", ""]
        for g in self.vulkan.get("gpus", []):
            lines += [f"GPU {g.get('index')}: {g.get('name')}", f"  arquitetura: {g.get('architecture')}", f"  Vulkan: {g.get('api_version')}", f"  VRAM local: {human_bytes(g.get('vram', 0))}", f"  tuning: {json.dumps(g.get('tuning', {}), ensure_ascii=False)}", ""]
        if not self.vulkan.get("available"): lines.append("Erro Vulkan: " + str(self.vulkan.get("error", "")))
        self.system_text.setPlainText("\n".join(lines)); self.refresh_library()

    def engine_runtime_report(self, exe: str, force: bool = False) -> dict[str, Any]:
        """Validate that Windows can load the engine before blaming shaders or the guest."""
        if not exe:
            return {"fatal": ["engine ausente"]}
        key = str(Path(exe).resolve())
        if force or key not in self.engine_runtime_cache:
            try:
                self.log(f"[ENGINE-DOCTOR] Validando runtime: {key}")
                report = engine_doctor.inspect(Path(key), do_fix=False, do_probe=(os.name == "nt"))
            except Exception as exc:
                report = {"fatal": [f"falha no diagnóstico da engine: {exc}"], "warnings": []}
            self.engine_runtime_cache[key] = report
            pe = report.get("pe") or {}
            probe = report.get("probe") or {}
            self.log(f"[ENGINE-DOCTOR] PE={pe.get('machine_name','?')} probe={probe.get('status','não executado')}")
            for w in report.get("warnings") or []:
                self.log(f"[ENGINE-DOCTOR] WARN: {w}")
            for e in report.get("fatal") or []:
                self.log(f"[ENGINE-DOCTOR] FATAL: {e}")
        return self.engine_runtime_cache[key]

    @staticmethod
    def inspect_guest_elf(path: Path) -> list[str]:
        """Reject obvious host-Linux ELFs while accepting ordinary x86-64 console/homebrew ELFs."""
        if not path.is_file() or path.suffix.lower() not in (".elf", ".bin"):
            return []
        try:
            head = path.read_bytes()[:2 * 1024 * 1024]
        except Exception:
            return []
        if len(head) < 20 or head[:4] != b"\x7fELF":
            return ["Arquivo .elf/.bin não possui cabeçalho ELF válido"] if path.suffix.lower() == ".elf" else []
        if head[4] != 2:
            return ["ELF não é 64-bit; PS4/PS5 usam x86-64"]
        machine = int.from_bytes(head[18:20], "little")
        if machine != 62:
            return [f"ELF usa e_machine={machine}; esperado x86-64 (62)"]
        linux_markers = (b"/lib64/ld-linux", b"/lib/x86_64-linux-gnu/ld-linux", b"libc.so.6", b"GNU/Linux")
        if any(m in head for m in linux_markers):
            return ["Este ELF parece um executável Linux de PC, não um ELF PS4/PS5/homebrew compatível"]
        return []

    def preflight(self, game: dict[str, Any]) -> list[str]:
        errors: list[str] = []
        p = Path(str(game.get("path", "")))
        if not p.exists(): errors.append("Arquivo/pasta do jogo não existe")
        if not self.vulkan.get("available"): errors.append("Vulkan loader não disponível")
        kind = str(game.get("kind", "")); platform_name = str(game.get("platform", "Auto")).upper()
        if kind == "pkg": errors.append("PKG bruto não é alvo de boot; instale/extraia o jogo primeiro")
        if platform_name == "PS4" and not self.engines.get("shadps4"): errors.append("Core PS4 integrado ausente")
        if platform_name != "PS4" and not (self.engines.get("kyty") or self.engines.get("sharpemu")): errors.append("Nenhum core PS5 encontrado: construa Kyty ou instale SharpEmu em engines/ps5/sharpemu")
        if p.is_dir() and kind != "pkg":
            if not (p / "eboot.bin").is_file() and not any(p.rglob("eboot.bin")):
                errors.append("Nenhum eboot.bin encontrado")
        elif p.is_file() and platform_name != "PS4":
            errors.extend(self.inspect_guest_elf(p))

        engine = self.engines.get("shadps4") if platform_name == "PS4" else (self.engines.get("kyty") or self.engines.get("sharpemu"))
        if engine:
            report = self.engine_runtime_report(engine)
            errors.extend(str(e) for e in (report.get("fatal") or []))
        return errors

    def build_command(self, game: dict[str, Any], safe: bool = False) -> tuple[str, list[str], str]:
        cfg = dict(self.cfg)
        override = str(game.get("profile_override", "global"))
        if override in core.PROFILE_OVERRIDES: cfg.update(core.PROFILE_OVERRIDES[override])
        p = Path(str(game.get("path", ""))); platform_name = str(game.get("platform", "Auto")).upper()
        if platform_name == "PS4":
            exe = self.engines["shadps4"]
            target = p / "eboot.bin" if p.is_dir() else p
            if p.is_dir() and not target.is_file(): target = next(p.rglob("eboot.bin"))
            args = [str(target)]
            options = core.engine_cli_options(exe)
            if cfg.get("fullscreen") and "--fullscreen" in options: args += ["--fullscreen", "true"]
            gpu = int(cfg.get("gpu_index", -1) or -1)
            if gpu >= 0:
                flag = next((f for f in ("--gpu-id", "--gpu", "--gpu_id") if f in options), "")
                if flag: args += [flag, str(gpu)]
            # Current shadPS4 has its own per-game keyboard/mouse mapper; physical Xbox/DS pads work out of box.
            return exe, args, "shadPS4"

        exe = self.engines["kyty"]
        if not exe:
            exe = self.engines["sharpemu"]
            if not exe:
                raise RuntimeError("Nenhum core PS5 disponível")
            target = p / "eboot.bin" if p.is_dir() else p
            if p.is_dir() and not target.is_file():
                target = next(p.rglob("eboot.bin"))
            return exe, [str(target)], "SharpEmu (experimental)"
        present = "Fifo" if safe else str(cfg.get("present_mode", "Fifo"))
        shader_opt = "None" if safe else str(cfg.get("shader_optimization", "Performance"))
        resolution = "1280x720" if safe else str(cfg.get("resolution", "1280x720"))
        args = ["--game", str(p), "--present-mode", present, "--shader-optimization-type", shader_opt,
                "--printf-direction", "Console", "--shader-log-direction", "Console"]
        m = re.fullmatch(r"(\d+)x(\d+)", resolution)
        if m: args += ["--screen-width", m.group(1), "--screen-height", m.group(2)]
        gpu = -1 if safe else int(cfg.get("gpu_index", -1) or -1)
        if gpu >= 0: args += ["--gpu", str(gpu)]
        args += ["--vblank-frequency", "60" if safe else str(max(30, min(360, int(cfg.get("vblank_frequency", 60) or 60))))]
        args += ["--controller-deadzone", str(max(0, min(40, int(cfg.get("controller_deadzone", 10) or 10))))]
        args += ["--user-name", (str(cfg.get("player_name", "Player1"))[:16] or "Player1")]
        if cfg.get("fullscreen"): args.append("--fullscreen")
        if os.name == "nt" and bool(cfg.get("redzone", True)): args.append("--redzone")
        if bool(cfg.get("playgo_hack", False)) or safe: args.append("--playgo-hack")
        for entry in list(cfg.get("keymap") or serialize_keymap(self.mapping, float(cfg.get("mouse_sensitivity", 1.0)))):
            args += ["--keymap", str(entry)]
        if safe:
            args += ["--readback-linear-images", "true"]
        return exe, args, "KytyPS5"

    def launch_game(self, game_id: str) -> None:
        if self.recovery_pending:
            QMessageBox.information(self, "Recuperação em andamento", "O HyperCore já está preparando a única tentativa segura. Aguarde um instante."); return
        if self.process is not None and self.process.state() != QProcess.ProcessState.NotRunning:
            QMessageBox.warning(self, "Jogo em execução", "Já existe uma engine ativa. Feche o jogo atual antes de iniciar outro."); return
        try:
            _items, _idx, game = core.game_by_id(game_id)
        except Exception as exc:
            QMessageBox.critical(self, "Biblioteca", str(exc)); return
        errors = self.preflight(game)
        if errors:
            text = "\n".join("• " + e for e in errors); self.log("[PREFLIGHT] BLOQUEADO\n" + text); QMessageBox.critical(self, "Pré-flight falhou", text); return
        self.current_game = game; self.current_safe_retry = False; self.recovery_pending = False; self.launch_serial += 1; self._start_process(safe=False)

    def _start_process(self, safe: bool) -> None:
        assert self.current_game is not None
        try:
            exe, args, engine = self.build_command(self.current_game, safe=safe)
        except Exception as exc:
            self.log(f"[BOOT] Falha ao montar comando: {exc}"); return
        self.current_safe_retry = safe; self.process_started = time.time(); self.log_buffer = self.log_buffer[-250:]; self.core_log_buffer = []
        gid = str(self.current_game.get("id")); suffix = "safe" if safe else "normal"; self.current_log_path = LOGS / f"boot-{gid}-{int(time.time())}-{suffix}.log"; self.current_log_file = self.current_log_path.open("a", encoding="utf-8", errors="replace")
        self.log(f"[BOOT] Engine={engine} modo={suffix}")
        self.log(f"[BOOT] CWD={Path(exe).parent}")
        self.log("[BOOT] CMD=" + subprocess.list2cmdline([exe] + args))
        proc = QProcess(self); proc.setProgram(exe); proc.setArguments(args); proc.setWorkingDirectory(str(Path(exe).parent)); proc.setProcessChannelMode(QProcess.ProcessChannelMode.MergedChannels)
        proc.readyReadStandardOutput.connect(self._process_output); proc.errorOccurred.connect(self._process_error); proc.finished.connect(self._process_finished)
        self.process = proc; self.status_dot.setText("● engine executando")
        self._hide_launcher_for_game()
        proc.start()

    def _hide_launcher_for_game(self) -> None:
        if self.launcher_hidden_for_game:
            return
        self.launcher_window_state = self.windowState()
        self.launcher_hidden_for_game = True
        self.hide()

    def _process_error(self, error: QProcess.ProcessError) -> None:
        self.log(f"[PROCESS] erro Qt={error.name}")
        if error == QProcess.ProcessError.FailedToStart:
            self.restore_launcher()

    def _process_output(self) -> None:
        if self.process is None: return
        data = bytes(self.process.readAllStandardOutput()).decode("utf-8", "replace")
        if not data: return
        if self.current_log_file:
            self.current_log_file.write(data); self.current_log_file.flush()
        self.core_log_buffer.append(data)
        if len(self.core_log_buffer) > 2000:
            self.core_log_buffer = self.core_log_buffer[-2000:]
        # Avoid QTextEdit making one paragraph per partial byte chunk.
        for line in data.rstrip("\n").splitlines(): self.log("[CORE] " + line)

    def diagnose_crash(self, text: str, code: int) -> tuple[str, bool]:
        status = engine_doctor.status_name(code)
        ucode = engine_doctor.u32(code)
        low = text.lower()
        if ucode == 0xC000007B:
            return ("Windows loader: 0xC000007B / STATUS_INVALID_IMAGE_FORMAT. A engine ou uma DLL é da arquitetura/formato errado. "
                    "Isso acontece antes de Vulkan/shaders; o HyperCore não fará retry gráfico inútil.", False)
        if ucode == 0xC0000135:
            return ("Windows loader: 0xC0000135 / STATUS_DLL_NOT_FOUND. Falta uma DLL/runtime exigido pela engine. "
                    "Repare a instalação do core/runtime antes de testar o jogo.", False)
        if ucode == 0xC0000142:
            return ("Windows loader: 0xC0000142 / STATUS_DLL_INIT_FAILED. Uma DLL foi encontrada mas falhou ao inicializar.", False)
        if ucode == 0xC0000005 or "access violation" in low or "0xc0000005" in low or "segmentation fault" in low:
            return "Violação de memória no core/guest. Preserve este log; precisa de correção no caminho que aparece antes do crash.", False
        if ucode == 0xC000001D:
            return "Instrução ilegal no host (STATUS_ILLEGAL_INSTRUCTION). Verifique build/CPU e código JIT antes de qualquer tuning gráfico.", False
        if "exit_not_implemented" in low or "not implemented" in low:
            return "O core atingiu uma função/instrução ainda não implementada. Retry de flags não corrige ausência de emulação.", False
        if "device lost" in low or "vk_error_device_lost" in low:
            return "GPU/Vulkan perdeu o dispositivo. O launcher tentará configuração conservadora uma vez.", True
        if "shader" in low or "pipeline" in low or "render" in low:
            return "O *output do core* aponta falha precoce no caminho gráfico/shader. Vale uma tentativa conservadora.", True
        if "playgo" in low or "chunk" in low:
            return "Falha relacionada a PlayGo/conteúdo parcial. Modo seguro habilita o fallback PlayGo.", True
        return f"A engine encerrou cedo sem assinatura conhecida ({status}). Um retry conservador pode separar configuração de bug real.", True

    def restore_launcher(self) -> None:
        if not self.launcher_hidden_for_game:
            return
        previous_state = self.launcher_window_state
        self.launcher_hidden_for_game = False
        if previous_state & Qt.WindowState.WindowMaximized:
            self.showMaximized()
        elif previous_state & Qt.WindowState.WindowFullScreen:
            self.showFullScreen()
        else:
            self.showNormal()
        self.raise_()
        self.activateWindow()
        QApplication.setActiveWindow(self)

    def _process_finished(self, code: int, _status: QProcess.ExitStatus) -> None:
        elapsed = time.time() - self.process_started
        self._process_output()
        if self.current_log_file:
            self.current_log_file.close(); self.current_log_file = None
        core_tail = "".join(self.core_log_buffer)[-60000:]
        tail = "\n".join(self.log_buffer[-400:])
        diagnosis, retryable = self.diagnose_crash(core_tail, code)
        self.log(f"[EXIT] código={code} hex=0x{engine_doctor.u32(code):08X} status={engine_doctor.status_name(code)} tempo={elapsed:.1f}s modo={'safe' if self.current_safe_retry else 'normal'}")
        self.log(f"[DIAG] {diagnosis}")
        self.status_dot.setText("● pronto" if code == 0 else "● crash detectado")
        if self.current_game:
            items = core.library()
            for i, g in enumerate(items):
                if g.get("id") == self.current_game.get("id"):
                    g["last_played"] = int(self.process_started); g["last_exit_code"] = int(code); g["status"] = "Encerrado normalmente" if code == 0 else f"Crash/exit {code} após {elapsed:.1f}s"; items[i] = g; break
            core.save_json(core.LIBRARY_FILE, items); self.refresh_library()
        early = code != 0 and elapsed < EARLY_CRASH_SECONDS
        if early and not self.current_safe_retry and bool(self.cfg.get("auto_safe_retry", True)) and retryable:
            self.recovery_pending = True
            serial = self.launch_serial
            self.process = None
            self.log("[RECOVERY] Crash precoce: uma única tentativa segura será feita em 1 segundo")
            def retry_once() -> None:
                if not self.recovery_pending or serial != self.launch_serial:
                    return
                self.recovery_pending = False
                if self.process is not None and self.process.state() != QProcess.ProcessState.NotRunning:
                    return
                self._start_process(safe=True)
            QTimer.singleShot(1000, retry_once)
            return
        self.recovery_pending = False
        if code != 0 and self.current_log_path:
            report = self.current_log_path.with_name("crash-report-" + self.current_log_path.name)
            report.write_text(f"PKG_DoW {APP_VERSION}\nexit={code}\nhex=0x{engine_doctor.u32(code):08X}\nstatus={engine_doctor.status_name(code)}\nelapsed={elapsed:.2f}\ndiagnosis={diagnosis}\nlog={self.current_log_path}\n\n=== CORE OUTPUT ===\n" + core_tail + "\n\n=== LAUNCHER TAIL ===\n" + tail, encoding="utf-8", errors="replace")
            self.log(f"[CRASH] Relatório: {report}")
        self.restore_launcher()
        self.process = None

    def closeEvent(self, event) -> None:  # noqa: N802
        if self.process is not None and self.process.state() != QProcess.ProcessState.NotRunning:
            answer = QMessageBox.question(self, "Engine ativa", "Encerrar também a engine/jogo?")
            if answer == QMessageBox.StandardButton.Yes:
                self.process.terminate(); self.process.waitForFinished(2500)
                if self.process.state() != QProcess.ProcessState.NotRunning: self.process.kill()
            else:
                event.ignore(); return
        event.accept()


def main() -> int:
    os.environ.setdefault("QT_ENABLE_HIGHDPI_SCALING", "1")
    app = QApplication(sys.argv); app.setApplicationName("PKG_DoW HyperCore"); app.setOrganizationName("PKG_DoW")
    win = HyperCoreWindow(); win.show(); return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
