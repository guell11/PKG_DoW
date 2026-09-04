#!/usr/bin/env python3
"""PKG_DoW local web launcher.

Zero third-party Python dependencies. It serves the HTML/CSS/JS dashboard, keeps a local
library, probes package/folder metadata, discovers emulator cores and launches extracted
PS4/PS5 titles through the appropriate executable when available.
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.util
import hashlib
import json
import mimetypes
import os
import platform
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
import traceback
import urllib.parse
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
WEB = ROOT / "webui"
DATA = ROOT / "userdata"
LOGS = ROOT / "logs"
LIBRARY_FILE = DATA / "library.json"
SETTINGS_FILE = DATA / "settings.json"
PS4_GAMES_DIR = DATA / "ps4-games"
PKG_TOOL_DIR = ROOT / "tools" / "pkgtool"
PKG_TOOL = PKG_TOOL_DIR / "PkgTool.Core.exe"

for d in (DATA, LOGS, PS4_GAMES_DIR):
    d.mkdir(parents=True, exist_ok=True)

DEFAULT_SETTINGS = {
    "player_name": "Player1",
    "kyty_path": "",
    "sharpemu_path": "",
    "shadps4_path": "",
    "performance_profile": "balanced",
    "present_mode": "Fifo",
    "shader_optimization": "Performance",
    "resolution": "1280x720",
    "fullscreen": False,
    "gpu_index": -1,
    "vblank_frequency": 60,
    "redzone": True,
    "playgo_hack": False,
    "ps4_boot_mode": "auto",
    "keyboard_mouse_enabled": True,
    "mouse_sensitivity": 1.25,
    "vulkan_validation": False,
    "gpu_assisted_validation": False,
    "shader_validation": False,
    "shader_log_direction": "Console",
    "printf_direction": "Console",
    "graphics_debug_dump": False,
    "command_buffer_dump": False,
    "readback_linear_images": False,
    "spirv_debug_printf": False,
    "host_cpu_affinity": [],
}

PROFILE_OVERRIDES: dict[str, dict[str, Any]] = {
    "stable": {"present_mode": "Fifo", "shader_optimization": "None",
               "resolution": "1280x720", "vblank_frequency": 60, "redzone": True},
    "balanced": {"present_mode": "Fifo", "shader_optimization": "Performance",
                 "resolution": "1280x720", "vblank_frequency": 60, "redzone": True},
    "turbo": {"present_mode": "Mailbox", "shader_optimization": "Performance",
              "resolution": "960x540", "vblank_frequency": 60, "redzone": True},
}

_lock = threading.RLock()
_processes: dict[str, subprocess.Popen] = {}
_install_jobs: dict[str, dict[str, Any]] = {}

PKG_DOW_INPUT_BEGIN = "# PKG_DoW keyboard/mouse profile begin"
PKG_DOW_INPUT_END = "# PKG_DoW keyboard/mouse profile end"


def configure_ps4_keyboard_mouse(exe: str, cfg: dict[str, Any]) -> Path:
    """Install PKG_DoW's unified keyboard/mouse-to-pad profile beside the PS4 core."""
    user_dir = Path(exe).resolve().parent / "user"
    input_dir = user_dir / "input_config"
    input_dir.mkdir(parents=True, exist_ok=True)
    profile = input_dir / "default.ini"
    text = profile.read_text(encoding="utf-8", errors="replace") if profile.is_file() else ""
    block_re = re.compile(
        rf"\n?{re.escape(PKG_DOW_INPUT_BEGIN)}.*?{re.escape(PKG_DOW_INPUT_END)}\n?",
        re.DOTALL,
    )
    text = block_re.sub("\n", text).rstrip()
    if bool(cfg.get("keyboard_mouse_enabled", True)):
        sensitivity = max(0.2, min(4.0, float(cfg.get("mouse_sensitivity", 1.25) or 1.25)))
        managed = f"""{PKG_DOW_INPUT_BEGIN}
# WASD -> analógico esquerdo
axis_left_x_minus = a
axis_left_x_plus = d
axis_left_y_minus = w
axis_left_y_plus = s

# Mouse -> analógico direito; botões -> gatilhos
mouse_to_joystick = right
mouse_movement_params = 0.35, {sensitivity:.2f}, 0.08
r2 = leftbutton
l2 = rightbutton
r3 = middlebutton
{PKG_DOW_INPUT_END}"""
        text = (text + "\n\n" + managed).lstrip()
    profile.write_text(text.rstrip() + "\n", encoding="utf-8")

    config_path = user_dir / "config.json"
    core_cfg = load_json(config_path, {})
    if not isinstance(core_cfg, dict):
        core_cfg = {}
    input_cfg = core_cfg.setdefault("Input", {})
    if not isinstance(input_cfg, dict):
        input_cfg = {}
        core_cfg["Input"] = input_cfg
    input_cfg["use_unified_input_config"] = True
    input_cfg["use_mice_as_mice"] = False
    save_json(config_path, core_cfg)
    return profile


def set_process_window_titles(pid: int, title: str) -> int:
    if os.name != "nt":
        return 0
    from ctypes import wintypes

    user32 = ctypes.windll.user32
    changed = 0
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user32.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
    user32.EnumWindows.restype = wintypes.BOOL
    user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user32.IsWindowVisible.argtypes = [wintypes.HWND]
    user32.SetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPCWSTR]

    @callback_type
    def visit(hwnd: int, _lparam: int) -> int:
        nonlocal changed
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            user32.SetWindowTextW(hwnd, title)
            changed += 1
        return 1

    user32.EnumWindows(visit, 0)
    return changed


def send_process_key(pid: int, virtual_key: int) -> int:
    if os.name != "nt":
        return 0
    from ctypes import wintypes

    user32 = ctypes.windll.user32
    sent = 0
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user32.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
    user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user32.IsWindowVisible.argtypes = [wintypes.HWND]
    user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]

    @callback_type
    def visit(hwnd: int, _lparam: int) -> int:
        nonlocal sent
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            user32.PostMessageW(hwnd, 0x0100, virtual_key, 0)  # WM_KEYDOWN
            user32.PostMessageW(hwnd, 0x0101, virtual_key, 0)  # WM_KEYUP
            sent += 1
        return 1

    user32.EnumWindows(visit, 0)
    return sent


def kyty_keymap_args(cfg: dict[str, Any]) -> list[str]:
    if not bool(cfg.get("keyboard_mouse_enabled", True)):
        return []
    sensitivity = max(0.2, min(4.0, float(cfg.get("mouse_sensitivity", 1.25) or 1.25)))
    bindings = [
        "LeftStickLeft=A", "LeftStickRight=D", "LeftStickUp=W", "LeftStickDown=S",
        "Cross=J", "Triangle=I", "Square=K", "Circle=L", "L1=Q", "R1=E",
        "L3=Left Shift", "Options=Return", "TouchPad=Backspace", "TouchPadRight=Tab",
        "R2=Mouse:Left", "L2=Mouse:Right", "R3=Mouse:Middle",
        f"MouseSensitivity={sensitivity:.2f}",
    ]
    return [part for binding in bindings for part in ("--keymap", binding)]


def brand_process_windows(
    proc: subprocess.Popen[Any], title: str, enable_mouse_capture: bool = False
) -> None:
    mouse_due = time.monotonic() + 1.0
    mouse_sent = not enable_mouse_capture
    while proc.poll() is None:
        windows = 0
        try:
            windows = set_process_window_titles(proc.pid, title)
            if not mouse_sent and windows and time.monotonic() >= mouse_due:
                mouse_sent = send_process_key(proc.pid, 0x76) > 0  # VK_F7
        except Exception:
            pass
        time.sleep(0.1 if not windows else 0.5)


def load_json(path: Path, fallback: Any) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return fallback


def save_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")
    tmp.replace(path)


def library() -> list[dict[str, Any]]:
    value = load_json(LIBRARY_FILE, [])
    return value if isinstance(value, list) else []


def settings() -> dict[str, Any]:
    value = load_json(SETTINGS_FILE, {})
    merged = dict(DEFAULT_SETTINGS)
    if isinstance(value, dict):
        merged.update(value)
    return merged


def stable_id(path: Path) -> str:
    return hashlib.sha1(str(path.resolve()).lower().encode("utf-8", "replace")).hexdigest()[:16]


def game_identity(path: Path, title_id: str) -> str:
    # PS4 title ID is the useful identity for library management: rescanning a moved game updates
    # its path instead of creating a second card for the same title.
    normalized = normalize_title_id(title_id) if "normalize_title_id" in globals() else str(title_id or "").upper()
    if re.fullmatch(r"(?!PPSA)[A-Z]{4}\d{5}", normalized):
        return hashlib.sha1(("ps4:" + normalized).encode("ascii")).hexdigest()[:16]
    return stable_id(path)


def parse_sfo(path: Path) -> dict[str, Any]:
    """Minimal PARAM.SFO reader for string/integer fields."""
    try:
        data = path.read_bytes()
        if len(data) < 20 or data[:4] != b"\x00PSF":
            return {}
        _, version, key_off, data_off, count = struct.unpack_from("<4sIIII", data, 0)
        out: dict[str, Any] = {}
        for i in range(count):
            off = 20 + i * 16
            if off + 16 > len(data):
                break
            key_idx, fmt, length, max_len, value_off = struct.unpack_from("<HHIII", data, off)
            key_pos = key_off + key_idx
            if key_pos >= len(data):
                continue
            key_end = data.find(b"\0", key_pos)
            if key_end < 0:
                continue
            key = data[key_pos:key_end].decode("utf-8", "replace")
            pos = data_off + value_off
            raw = data[pos:pos + min(length, max_len)] if pos < len(data) else b""
            if fmt == 0x0204:
                out[key] = raw.split(b"\0", 1)[0].decode("utf-8", "replace")
            elif fmt == 0x0404 and len(raw) >= 4:
                out[key] = int.from_bytes(raw[:4], "little")
            else:
                text = raw.split(b"\0", 1)[0].decode("utf-8", "replace").strip()
                if text:
                    out[key] = text
        return out
    except OSError:
        return {}


def inspect_pkg_container(path: Path) -> dict[str, Any]:
    """Reads public PS4 CNT fields only. Never derives keys or decrypts PFS."""
    info: dict[str, Any] = {
        "pkg_valid": False, "pkg_entries": 0, "pkg_pfs_size": 0,
        "pkg_encrypted": False, "pkg_direct_extractable": False,
    }
    try:
        size = path.stat().st_size
        with path.open("rb") as stream:
            header = stream.read(0x5A0)
            if len(header) < 0x5A0 or header[:4] != b"\x7fCNT":
                return info
            count = struct.unpack_from(">I", header, 0x10)[0]
            count_copy = struct.unpack_from(">H", header, 0x16)[0]
            table_offset = struct.unpack_from(">I", header, 0x18)[0]
            pfs_size = struct.unpack_from(">Q", header, 0x418)[0]
            if count == 0 or count > 16384 or count_copy != count:
                return info
            table_size = count * 0x20
            if table_offset > size or table_size > size - table_offset:
                return info
            stream.seek(table_offset)
            table = stream.read(table_size)
            if len(table) != table_size:
                return info
        encrypted = any(
            struct.unpack_from(">I", table, index * 0x20 + 8)[0] & 0x80000000
            for index in range(count)
        )
        content_id = header[0x40:0x70].split(b"\0", 1)[0].decode("ascii", "replace")
        match = re.search(r"-([A-Z]{4}\d{5})_", content_id)
        info.update({
            "pkg_valid": True,
            "pkg_entries": count,
            "pkg_pfs_size": pfs_size,
            "pkg_encrypted": bool(encrypted or pfs_size),
            "pkg_direct_extractable": not encrypted and pfs_size == 0,
            "content_id": content_id,
            "title_id": match.group(1) if match else "",
            "pkg_drm_type": struct.unpack_from(">I", header, 0x70)[0],
            "pkg_content_type": struct.unpack_from(">I", header, 0x74)[0],
        })
    except (OSError, struct.error):
        pass
    return info


def pkg_metadata(path: Path) -> dict[str, Any]:
    """Non-decrypting metadata probe using plain strings present in Sony-style PKG headers/tables."""
    meta: dict[str, Any] = inspect_pkg_container(path)
    try:
        with path.open("rb") as f:
            chunks = []
            remaining = min(path.stat().st_size, 64 * 1024 * 1024)
            while remaining > 0:
                part = f.read(min(4 * 1024 * 1024, remaining))
                if not part:
                    break
                chunks.append(part)
                remaining -= len(part)
        data = b"".join(chunks)
        text = data.decode("latin-1", "ignore")
        content = re.search(r"[A-Z]{2}\d{4}-([A-Z]{4}\d{5})_00-[A-Z0-9]{16}", text)
        if content:
            meta["content_id"] = content.group(0)
            meta["title_id"] = content.group(1)
        if "title_id" not in meta:
            ids = re.findall(r"\b(?:CUSA|PPSA|LAPY|NPXS|NPXX)\d{5}\b", text)
            if ids:
                meta["title_id"] = ids[0]
        # Homebrew packages often expose TITLE nearby as an ASCII value. Prefer recognizable text.
        for m in re.finditer(r"(?:PS4|PS5)[ -][A-Za-z0-9][A-Za-z0-9 ._:'&+\-]{2,60}", text):
            candidate = m.group(0).strip("\0 ._")
            if 4 <= len(candidate) <= 64:
                meta["title"] = candidate
                break
    except OSError:
        pass
    return meta


CUSA_RE = re.compile(r"^CUSA\d{5}$", re.IGNORECASE)
TITLE_ID_RE = re.compile(r"\b[A-Z]{4}\d{5}\b", re.IGNORECASE)


def normalize_title_id(value: Any) -> str:
    text = str(value or "").strip().upper()
    match = TITLE_ID_RE.search(text)
    return match.group(0).upper() if match else text[:16]


def infer_title_id_from_path(path: Path) -> str:
    candidates = [path.stem, path.name]
    try:
        candidates += [path.parent.name, path.parent.parent.name]
    except Exception:
        pass
    for candidate in candidates:
        match = TITLE_ID_RE.search(str(candidate).upper())
        if match:
            return match.group(0).upper()
    return ""


def content_region(content_id: str) -> str:
    prefix = str(content_id or "").upper()[:2]
    return {
        "UP": "Américas", "EP": "Europa", "JP": "Japão", "HP": "Ásia",
        "KP": "Coreia", "IP": "Internacional",
    }.get(prefix, "")


def format_system_version(value: Any) -> str:
    try:
        num = int(value)
    except (TypeError, ValueError):
        return str(value or "")
    if num <= 0:
        return ""
    # PARAM.SFO system versions are conventionally encoded as packed hexadecimal bytes.
    text = f"{num:08X}"
    return f"{int(text[0:2],16)}.{text[2:4]}.{text[4:6]}"


def infer_platform(path: Path, meta: dict[str, Any]) -> str:
    title_id = str(meta.get("TITLE_ID") or meta.get("title_id") or "").upper()
    title = str(meta.get("TITLE") or meta.get("title") or "").upper()
    if title_id.startswith("PPSA") or "PS5" in title:
        return "PS5"
    if ((TITLE_ID_RE.fullmatch(title_id) and not title_id.startswith("PPSA")) or
            "PS4" in title):
        return "PS4"
    if path.suffix.lower() == ".pkg":
        return "PKG"
    return "Auto"


def find_sfo(base: Path) -> Path | None:
    candidates = [base / "sce_sys" / "param.sfo", base / "param.sfo"]
    return next((p for p in candidates if p.is_file()), None)


def find_icon(base: Path) -> str:
    for rel in ("sce_sys/icon0.png", "icon0.png"):
        p = base / rel
        if p.is_file():
            try:
                return "/api/file?path=" + urllib.parse.quote(str(p.resolve()))
            except OSError:
                pass
    return ""


def describe_import(raw_path: str) -> dict[str, Any]:
    p = Path(raw_path).expanduser()
    if not p.exists():
        raise FileNotFoundError(str(p))
    p = p.resolve()
    meta: dict[str, Any] = {}
    kind = "folder" if p.is_dir() else "file"
    icon = ""
    if p.is_dir():
        sfo = find_sfo(p)
        if sfo:
            meta.update(parse_sfo(sfo))
        icon = find_icon(p)
    elif p.suffix.lower() == ".pkg":
        kind = "pkg"
        meta.update(pkg_metadata(p))
    elif p.name.lower() == "eboot.bin" or p.suffix.lower() in {".elf", ".bin"}:
        kind = "elf"
        sfo = find_sfo(p.parent)
        if sfo:
            meta.update(parse_sfo(sfo))
        icon = find_icon(p.parent)

    title = str(meta.get("TITLE") or meta.get("title") or p.stem or p.name)
    title_id = normalize_title_id(meta.get("TITLE_ID") or meta.get("title_id") or infer_title_id_from_path(p))
    platform_name = infer_platform(p, meta)
    size = p.stat().st_size if p.is_file() else 0
    status = "Pronto"
    if kind == "pkg":
        status = "Pronto para instalar" if meta.get("pkg_valid") and platform_name == "PS4" else "PKG não suportado"
    return {
        "id": game_identity(p, title_id), "path": str(p), "title": title, "title_id": title_id,
        "content_id": str(meta.get("CONTENT_ID") or meta.get("content_id") or ""), "platform": platform_name,
        "kind": kind, "size": size, "favorite": False, "last_played": 0,
        "play_seconds": 0, "added_at": int(time.time()), "icon": icon,
        "status": status,
        "profile_override": "global", "ps4_boot_mode": "auto",
        "app_ver": str(meta.get("APP_VER") or meta.get("VERSION") or ""),
        "category": str(meta.get("CATEGORY") or ""),
        "system_ver": format_system_version(meta.get("SYSTEM_VER") or meta.get("SYSTEM_VER_REQUIRED") or ""),
        "region": content_region(str(meta.get("CONTENT_ID") or meta.get("content_id") or "")),
        "cusa": bool(CUSA_RE.fullmatch(title_id)),
        "pkg_valid": bool(meta.get("pkg_valid", False)),
        "pkg_entries": int(meta.get("pkg_entries", 0) or 0),
        "pkg_pfs_size": int(meta.get("pkg_pfs_size", 0) or 0),
        "pkg_encrypted": bool(meta.get("pkg_encrypted", False)),
        "pkg_direct_extractable": bool(meta.get("pkg_direct_extractable", False)),
    }


def upsert_game(item: dict[str, Any]) -> dict[str, Any]:
    with _lock:
        items = library()
        item_path = os.path.normcase(os.path.abspath(str(item.get("path", ""))))
        matches = [
            i for i, old in enumerate(items)
            if old.get("id") == item["id"] or (
                item_path and os.path.normcase(os.path.abspath(str(old.get("path", "")))) == item_path
            )
        ]
        if matches:
            old_items = [items[i] for i in matches]
            item["favorite"] = any(bool(old.get("favorite")) for old in old_items)
            item["last_played"] = max(int(old.get("last_played", 0) or 0) for old in old_items)
            item["play_seconds"] = max(int(old.get("play_seconds", 0) or 0) for old in old_items)
            added = [int(old.get("added_at", 0) or 0) for old in old_items if old.get("added_at")]
            if added:
                item["added_at"] = min(added)
            for key in ("profile_override", "ps4_boot_mode"):
                chosen = next((old.get(key) for old in reversed(old_items) if old.get(key)), None)
                if chosen is not None:
                    item[key] = chosen
            first = matches[0]
            items = [old for i, old in enumerate(items) if i not in set(matches)]
            items.insert(first, item)
            save_json(LIBRARY_FILE, items)
            return item
        items.append(item)
        save_json(LIBRARY_FILE, items)
        return item


def discover_executable(configured: str, candidates: list[Path]) -> str:
    if configured:
        p = Path(configured).expanduser()
        if p.is_file():
            return str(p.resolve())
    for p in candidates:
        if p.is_file():
            return str(p.resolve())
    return ""


def engine_paths() -> dict[str, str]:
    cfg = settings()
    kyty = discover_executable(str(cfg.get("kyty_path", "")), [
        ROOT / "_Build/windows/install/kyty_emulator.exe",
        ROOT / "_Build/windows/install/bin/kyty_emulator.exe",
        ROOT / "_Build/windows/kyty_emulator.exe",
        ROOT / "bin/kyty_emulator.exe",
        ROOT / "kyty_emulator.exe",
    ])
    shad = discover_executable(str(cfg.get("shadps4_path", "")), [
        ROOT / "engines/ps4/core-0.18.0/shadPS4.exe",
        ROOT / "engines/ps4/core-0.18.0/shadps4.exe",
        ROOT / "engines/ps4/shadPS4.exe",
        ROOT / "engines/ps4/shadps4.exe",
        ROOT / "engines/shadps4/shadPS4.exe",
        ROOT / "engines/shadps4/shadps4.exe",
        ROOT / "shadPS4.exe",
        ROOT / "shadps4.exe",
    ])
    sharp = discover_executable(str(cfg.get("sharpemu_path", "")), [
        ROOT / "engines/ps5/sharpemu/SharpEmu.exe",
        ROOT / "engines/ps5/sharpemu/sharpemu.exe",
        ROOT / "engines/ps5/SharpEmu.exe",
        ROOT / "engines/sharpemu/SharpEmu.exe",
        ROOT / "SharpEmu/SharpEmu.exe",
        ROOT / "sharpemu/SharpEmu.exe",
    ])
    pkg_installer = str(PKG_TOOL) if PKG_TOOL.is_file() else ""
    return {"kyty": kyty, "sharpemu": sharp, "shadps4": shad,
            "pkg_installer": pkg_installer}


_vulkan_probe_cache: tuple[float, dict[str, Any]] | None = None
_engine_help_cache: dict[str, tuple[float, float, set[str]]] = {}


def _vk_version(value: int) -> str:
    return f"{(value >> 22) & 0x3ff}.{(value >> 12) & 0x3ff}.{value & 0xfff}"


def classify_host_gpu(name: str, vendor_id: int) -> str:
    """Classify host GPU generation for policy tuning, never for native ISA emission."""
    n = str(name or "").upper()
    if int(vendor_id or 0) == 0x10DE:
        if "RTX 50" in n:
            return "NVIDIA Blackwell"
        if "RTX 40" in n:
            return "NVIDIA Ada Lovelace"
        if "RTX 30" in n:
            return "NVIDIA Ampere"
        if "RTX 20" in n or "GTX 16" in n:
            return "NVIDIA Turing"
        return "NVIDIA Vulkan"
    if int(vendor_id or 0) == 0x1002:
        if any(token in n for token in ("RX 9", "RX 90")):
            return "AMD RDNA 4"
        if any(token in n for token in ("RX 7", "RX 70")):
            return "AMD RDNA 3"
        if any(token in n for token in ("RX 6", "RX 60")):
            return "AMD RDNA 2"
        return "AMD Vulkan"
    if int(vendor_id or 0) == 0x8086:
        return "Intel Vulkan"
    return "Generic Vulkan"


def adaptive_vram_policy(vram_bytes: int, device_type: str = "Dedicada") -> dict[str, Any]:
    """Mirror the native core's safe-residency policy for UX/diagnostics."""
    gib = 1024 ** 3
    mib = 1024 ** 2
    vram = max(0, int(vram_bytes or 0))
    dedicated = str(device_type).lower().startswith("dedic")
    if not dedicated:
        target_ratio, pressure_ratio, critical_ratio, reserve = 0.70, 0.82, 0.90, 2 * gib
    elif vram <= 6 * gib:
        target_ratio, pressure_ratio, critical_ratio, reserve = 0.82, 0.89, 0.95, 768 * mib
    elif vram <= 8 * gib:
        target_ratio, pressure_ratio, critical_ratio, reserve = 0.86, 0.92, 0.96, 640 * mib
    elif vram <= 12 * gib:
        target_ratio, pressure_ratio, critical_ratio, reserve = 0.90, 0.94, 0.97, 768 * mib
    elif vram <= 16 * gib:
        target_ratio, pressure_ratio, critical_ratio, reserve = 0.91, 0.95, 0.975, 1024 * mib
    else:
        target_ratio, pressure_ratio, critical_ratio, reserve = 0.92, 0.96, 0.98, 1280 * mib
    reserve = min(reserve, vram // 4) if vram else 0
    target = min(int(vram * target_ratio), max(0, vram - reserve))
    return {
        "mode": "adaptive",
        "target_ratio": target_ratio,
        "pressure_ratio": pressure_ratio,
        "critical_ratio": critical_ratio,
        "reserve": reserve,
        "target": target,
    }


def _load_vulkan_library() -> Any:
    names: list[str] = []
    if os.name == "nt":
        names = [str(Path(os.environ.get("WINDIR", r"C:\\Windows")) / "System32" / "vulkan-1.dll"), "vulkan-1.dll"]
    elif sys.platform == "darwin":
        names = ["libvulkan.1.dylib", "libvulkan.dylib", "libMoltenVK.dylib"]
    else:
        found = ctypes.util.find_library("vulkan")
        names = [x for x in (found, "libvulkan.so.1", "libvulkan.so") if x]
    for name in names:
        try:
            return ctypes.CDLL(name)
        except OSError:
            continue
    return None


def vulkan_probe(force: bool = False) -> dict[str, Any]:
    """Probe the Vulkan loader and adapters without requiring the Vulkan SDK or Python packages."""
    global _vulkan_probe_cache
    now = time.time()
    if not force and _vulkan_probe_cache and now - _vulkan_probe_cache[0] < 30:
        return _vulkan_probe_cache[1]

    result: dict[str, Any] = {"available": False, "loader_version": "", "gpus": [], "error": ""}
    lib = _load_vulkan_library()
    if lib is None:
        result["error"] = "Vulkan loader não encontrado"
        _vulkan_probe_cache = (now, result)
        return result

    VkInstance = ctypes.c_void_p
    VkPhysicalDevice = ctypes.c_void_p
    VkResult = ctypes.c_int32
    VK_SUCCESS = 0
    VK_STRUCTURE_TYPE_APPLICATION_INFO = 0
    VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1
    VK_API_VERSION_1_0 = 1 << 22
    VK_API_VERSION_1_3 = (1 << 22) | (3 << 12)
    VK_MEMORY_HEAP_DEVICE_LOCAL_BIT = 0x1

    class VkApplicationInfo(ctypes.Structure):
        _fields_ = [("sType", ctypes.c_uint32), ("pNext", ctypes.c_void_p),
                    ("pApplicationName", ctypes.c_char_p), ("applicationVersion", ctypes.c_uint32),
                    ("pEngineName", ctypes.c_char_p), ("engineVersion", ctypes.c_uint32),
                    ("apiVersion", ctypes.c_uint32)]

    class VkInstanceCreateInfo(ctypes.Structure):
        _fields_ = [("sType", ctypes.c_uint32), ("pNext", ctypes.c_void_p), ("flags", ctypes.c_uint32),
                    ("pApplicationInfo", ctypes.POINTER(VkApplicationInfo)),
                    ("enabledLayerCount", ctypes.c_uint32), ("ppEnabledLayerNames", ctypes.c_void_p),
                    ("enabledExtensionCount", ctypes.c_uint32), ("ppEnabledExtensionNames", ctypes.c_void_p)]

    class VkPhysicalDeviceProperties(ctypes.Structure):
        _fields_ = [("apiVersion", ctypes.c_uint32), ("driverVersion", ctypes.c_uint32),
                    ("vendorID", ctypes.c_uint32), ("deviceID", ctypes.c_uint32),
                    ("deviceType", ctypes.c_uint32), ("deviceName", ctypes.c_char * 256),
                    ("pipelineCacheUUID", ctypes.c_uint8 * 16), ("_tail", ctypes.c_uint8 * 4096)]

    class VkExtensionProperties(ctypes.Structure):
        _fields_ = [("extensionName", ctypes.c_char * 256), ("specVersion", ctypes.c_uint32)]

    class VkMemoryType(ctypes.Structure):
        _fields_ = [("propertyFlags", ctypes.c_uint32), ("heapIndex", ctypes.c_uint32)]

    class VkMemoryHeap(ctypes.Structure):
        _fields_ = [("size", ctypes.c_uint64), ("flags", ctypes.c_uint32)]

    class VkPhysicalDeviceMemoryProperties(ctypes.Structure):
        _fields_ = [("memoryTypeCount", ctypes.c_uint32), ("memoryTypes", VkMemoryType * 32),
                    ("memoryHeapCount", ctypes.c_uint32), ("memoryHeaps", VkMemoryHeap * 16)]

    instance = VkInstance()
    try:
        loader_version = VK_API_VERSION_1_0
        enum_ver = getattr(lib, "vkEnumerateInstanceVersion", None)
        if enum_ver is not None:
            enum_ver.argtypes = [ctypes.POINTER(ctypes.c_uint32)]
            enum_ver.restype = VkResult
            version_out = ctypes.c_uint32()
            if enum_ver(ctypes.byref(version_out)) == VK_SUCCESS:
                loader_version = int(version_out.value)
        result["loader_version"] = _vk_version(loader_version)
        app = VkApplicationInfo(VK_STRUCTURE_TYPE_APPLICATION_INFO, None, b"PKG_DoW", 0, b"PKG_DoW", 0,
                                min(loader_version, VK_API_VERSION_1_3))
        create = VkInstanceCreateInfo(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, None, 0, ctypes.pointer(app), 0, None, 0, None)
        lib.vkCreateInstance.argtypes = [ctypes.POINTER(VkInstanceCreateInfo), ctypes.c_void_p, ctypes.POINTER(VkInstance)]
        lib.vkCreateInstance.restype = VkResult
        rc = int(lib.vkCreateInstance(ctypes.byref(create), None, ctypes.byref(instance)))
        if rc != VK_SUCCESS or not instance:
            result["error"] = f"vkCreateInstance falhou ({rc})"
            return result

        lib.vkEnumeratePhysicalDevices.argtypes = [VkInstance, ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(VkPhysicalDevice)]
        lib.vkEnumeratePhysicalDevices.restype = VkResult
        count = ctypes.c_uint32()
        rc = int(lib.vkEnumeratePhysicalDevices(instance, ctypes.byref(count), None))
        if rc != VK_SUCCESS or count.value == 0:
            result["error"] = "Nenhum adaptador Vulkan encontrado"
            return result
        devices = (VkPhysicalDevice * count.value)()
        rc = int(lib.vkEnumeratePhysicalDevices(instance, ctypes.byref(count), devices))
        if rc != VK_SUCCESS:
            result["error"] = f"vkEnumeratePhysicalDevices falhou ({rc})"
            return result

        lib.vkGetPhysicalDeviceProperties.argtypes = [VkPhysicalDevice, ctypes.POINTER(VkPhysicalDeviceProperties)]
        lib.vkGetPhysicalDeviceProperties.restype = None
        lib.vkGetPhysicalDeviceMemoryProperties.argtypes = [VkPhysicalDevice, ctypes.POINTER(VkPhysicalDeviceMemoryProperties)]
        lib.vkGetPhysicalDeviceMemoryProperties.restype = None
        lib.vkEnumerateDeviceExtensionProperties.argtypes = [VkPhysicalDevice, ctypes.c_char_p,
                                                               ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(VkExtensionProperties)]
        lib.vkEnumerateDeviceExtensionProperties.restype = VkResult
        type_names = {0: "Outro", 1: "Integrada", 2: "Dedicada", 3: "Virtual", 4: "CPU"}
        gpus: list[dict[str, Any]] = []
        for index, dev in enumerate(devices):
            props = VkPhysicalDeviceProperties(); lib.vkGetPhysicalDeviceProperties(dev, ctypes.byref(props))
            mem = VkPhysicalDeviceMemoryProperties(); lib.vkGetPhysicalDeviceMemoryProperties(dev, ctypes.byref(mem))
            local_bytes = 0
            for i in range(min(int(mem.memoryHeapCount), 16)):
                heap = mem.memoryHeaps[i]
                if int(heap.flags) & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT:
                    local_bytes += int(heap.size)
            ext_count = ctypes.c_uint32()
            extensions: set[str] = set()
            if lib.vkEnumerateDeviceExtensionProperties(dev, None, ctypes.byref(ext_count), None) == VK_SUCCESS and ext_count.value:
                exts = (VkExtensionProperties * ext_count.value)()
                if lib.vkEnumerateDeviceExtensionProperties(dev, None, ctypes.byref(ext_count), exts) == VK_SUCCESS:
                    for ext in exts:
                        extensions.add(bytes(ext.extensionName).split(b"\0", 1)[0].decode("ascii", "ignore"))
            api_value = int(props.apiVersion)
            gpu_name = bytes(props.deviceName).split(b"\0", 1)[0].decode("utf-8", "replace")
            gpu_type = type_names.get(int(props.deviceType), "Outro")
            architecture = classify_host_gpu(gpu_name, int(props.vendorID))
            tuning = adaptive_vram_policy(local_bytes, gpu_type)
            tuning.update({
                "descriptor_cache_entries": 8192 if int(props.vendorID) == 0x10DE else 4096,
                "pipeline_compile": "background-safe",
                "upload_policy": "no-stall-ring+spill",
                "barrier_policy": "read-read-elision",
            })
            gpus.append({
                "index": index,
                "name": gpu_name,
                "type": gpu_type,
                "architecture": architecture,
                "api_version": _vk_version(api_value), "api_version_raw": api_value,
                "driver_version": int(props.driverVersion), "vendor_id": int(props.vendorID), "device_id": int(props.deviceID),
                "vram": local_bytes,
                "tuning": tuning,
                "translation_path": "Prospero/RDNA2 -> IR -> SPIR-V -> Vulkan -> driver nativo",
                "swapchain": "VK_KHR_swapchain" in extensions,
                "push_descriptor": "VK_KHR_push_descriptor" in extensions,
                "memory_budget": "VK_EXT_memory_budget" in extensions,
                "memory_priority": "VK_EXT_memory_priority" in extensions,
                "pipeline_cache_control": "VK_EXT_pipeline_creation_cache_control" in extensions,
                "descriptor_buffer": "VK_EXT_descriptor_buffer" in extensions,
                "shadps4_ready": api_value >= VK_API_VERSION_1_3 and "VK_KHR_swapchain" in extensions and "VK_KHR_push_descriptor" in extensions,
            })
        result["gpus"] = gpus
        result["available"] = bool(gpus)
        return result
    except Exception as exc:
        result["error"] = f"Falha no probe Vulkan: {exc}"
        return result
    finally:
        if instance:
            try:
                lib.vkDestroyInstance.argtypes = [VkInstance, ctypes.c_void_p]
                lib.vkDestroyInstance.restype = None
                lib.vkDestroyInstance(instance, None)
            except Exception:
                pass
        _vulkan_probe_cache = (now, result)


def vulkan_runtime_available() -> bool:
    return bool(vulkan_probe().get("available"))


def engine_cli_options(exe: str) -> set[str]:
    """Discover command line flags from --help so launcher integration follows the installed core version."""
    if not exe:
        return set()
    try:
        mtime = Path(exe).stat().st_mtime
    except OSError:
        return set()
    cached = _engine_help_cache.get(exe)
    if cached and cached[0] == mtime and time.time() - cached[1] < 300:
        return cached[2]
    options: set[str] = set()
    try:
        cp = subprocess.run([exe, "--help"], cwd=str(Path(exe).parent), capture_output=True,
                            text=True, errors="replace", timeout=5, check=False)
        text = (cp.stdout or "") + "\n" + (cp.stderr or "")
        options.update(re.findall(r"(?<!\w)--[A-Za-z0-9][A-Za-z0-9_-]*", text))
        options.update(re.findall(r"(?<!\w)-[A-Za-z](?=\s|,|$)", text))
    except (OSError, subprocess.SubprocessError):
        pass
    _engine_help_cache[exe] = (mtime, time.time(), options)
    return options


def scan_cusa_root(raw_path: str, max_games: int = 500) -> list[dict[str, Any]]:
    root = Path(raw_path).expanduser().resolve()
    if not root.is_dir():
        raise ValueError("A raiz PS4 precisa ser uma pasta")
    found: list[dict[str, Any]] = []
    for current, dirs, files in os.walk(root):
        here = Path(current)
        has_sfo = (here / "sce_sys" / "param.sfo").is_file() or (here / "param.sfo").is_file()
        has_eboot = "eboot.bin" in {name.lower() for name in files}
        path_title = infer_title_id_from_path(here)
        if has_sfo or (has_eboot and CUSA_RE.fullmatch(path_title or "")):
            try:
                game = describe_import(str(here))
                if game.get("platform") == "PS4" or game.get("cusa"):
                    found.append(upsert_game(game))
            except (OSError, ValueError):
                pass
            dirs[:] = []
            if len(found) >= max_games:
                break
        # Skip obviously huge/generated trees which cannot be game roots.
        dirs[:] = [d for d in dirs if d.lower() not in {"_sce_module", "sce_module", "data", "cache", "shadercache", "trophy2"}]
    return found


def sync_ps4_installs() -> list[dict[str, Any]]:
    """Imports PS4 folders created by package installer into PKG_DoW library."""
    PS4_GAMES_DIR.mkdir(parents=True, exist_ok=True)
    return scan_cusa_root(str(PS4_GAMES_DIR))


def _register_ps4_game_root(executable: str) -> None:
    if not executable:
        return
    exe = Path(executable)
    exe.parent.joinpath("user").mkdir(parents=True, exist_ok=True)
    PS4_GAMES_DIR.mkdir(parents=True, exist_ok=True)
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0) if os.name == "nt" else 0
    completed = subprocess.run(
        [str(exe), "--add-game-folder", str(PS4_GAMES_DIR)],
        cwd=str(exe.parent), capture_output=True, text=True, errors="replace",
        timeout=15, check=False, creationflags=flags,
    )
    if completed.returncode != 0:
        detail = ((completed.stdout or "") + "\n" + (completed.stderr or "")).strip()
        raise RuntimeError(f"Falha ao configurar pasta PS4 ({completed.returncode}): {detail[-800:]}")


def _safe_install_temp(path: Path) -> bool:
    try:
        return path.resolve().parent == PS4_GAMES_DIR.resolve() and path.name.startswith(".install-")
    except OSError:
        return False


def _finish_pkg_install(job_id: str, package_game: dict[str, Any], target: Path,
                        temp_dir: Path, process: subprocess.Popen[Any], log_handle: Any) -> None:
    package_size = max(1, int(package_game.get("size", 0) or 0))
    try:
        while process.poll() is None:
            extracted = directory_size(temp_dir)
            with _lock:
                if job_id in _install_jobs:
                    _install_jobs[job_id]["progress"] = min(94, max(2, int(extracted * 100 / package_size)))
                    _install_jobs[job_id]["extracted"] = extracted
            time.sleep(.35)
        log_handle.close()
        if process.returncode != 0:
            detail = ""
            log_path = LOGS / f"pkg-install-{job_id}.log"
            try:
                detail = log_path.read_text(encoding="utf-8", errors="replace")[-1200:]
            except OSError:
                pass
            raise RuntimeError(f"Falha ao extrair PKG ({process.returncode}): {detail.strip()}")
        extracted_root = temp_dir / "uroot"
        if not extracted_root.is_dir():
            extracted_root = temp_dir
        if not (extracted_root / "eboot.bin").is_file():
            raise RuntimeError("PKG extraído sem eboot.bin")
        target.mkdir(parents=True, exist_ok=False)
        shutil.copytree(extracted_root, target, dirs_exist_ok=True)
        installed = describe_import(str(target))
        for key in ("title", "title_id", "content_id", "app_ver", "region"):
            if package_game.get(key):
                installed[key] = package_game[key]
        installed.update({"platform": "PS4", "kind": "folder", "cusa": True,
                          "status": "Instalado", "size": directory_size(target)})
        installed["id"] = game_identity(target, title_id)
        icon = find_icon(target)
        if icon:
            installed["icon"] = icon
        installed = upsert_game(installed)
        with _lock:
            _install_jobs[job_id].update({"state": "done", "progress": 100,
                                          "game": installed, "path": str(target)})
    except Exception as exc:
        with _lock:
            if job_id in _install_jobs:
                _install_jobs[job_id].update({"state": "error", "error": str(exc)})
    finally:
        try:
            if not log_handle.closed:
                log_handle.close()
        except Exception:
            pass
        if _safe_install_temp(temp_dir):
            shutil.rmtree(temp_dir, ignore_errors=True)


def start_pkg_install(game_id: str) -> dict[str, Any]:
    """Extract a PS4 PKG through a hidden CLI process. Never launches emulator GUI."""
    _, _, game = game_by_id(game_id)
    package = Path(str(game.get("path", ""))).resolve()
    if game.get("kind") != "pkg" or not package.is_file() or package.suffix.lower() != ".pkg":
        raise ValueError("Jogo selecionado não é PKG válido")
    if not inspect_pkg_container(package).get("pkg_valid"):
        raise ValueError("PKG PS4 inválido ou truncado")
    if not PKG_TOOL.is_file():
        raise RuntimeError("Extrator PKG interno ausente")
    title_id = normalize_title_id(game.get("title_id")) or f"PKG{stable_id(package)[:9].upper()}"
    title_id = re.sub(r"[^A-Z0-9_-]", "", title_id)[:32]
    target = PS4_GAMES_DIR / title_id
    if target.exists():
        raise FileExistsError(f"{title_id} já está instalado. Remova pasta instalada antes de reinstalar.")
    job_id = hashlib.sha1(f"{package}:{time.time_ns()}".encode()).hexdigest()[:14]
    temp_dir = PS4_GAMES_DIR / f".install-{job_id}"
    temp_dir.mkdir(parents=True, exist_ok=False)
    log_path = LOGS / f"pkg-install-{job_id}.log"
    log_handle = log_path.open("w", encoding="utf-8", errors="replace")
    env = os.environ.copy(); env["DOTNET_ROLL_FORWARD"] = "Major"
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0) if os.name == "nt" else 0
    try:
        process = subprocess.Popen(
            [str(PKG_TOOL), "pkg_extract", str(package), str(temp_dir)],
            cwd=str(PKG_TOOL_DIR), stdout=log_handle, stderr=subprocess.STDOUT,
            creationflags=flags, env=env,
        )
    except Exception:
        log_handle.close()
        if _safe_install_temp(temp_dir):
            shutil.rmtree(temp_dir, ignore_errors=True)
        raise
    job = {"ok": True, "job": job_id, "state": "running", "progress": 1,
           "title": game.get("title") or title_id, "title_id": title_id,
           "package": str(package), "target": str(target), "pid": process.pid}
    with _lock:
        _install_jobs[job_id] = job
    threading.Thread(target=_finish_pkg_install,
                     args=(job_id, dict(game), target, temp_dir, process, log_handle),
                     daemon=True).start()
    return dict(job)


def install_status(job_id: str) -> dict[str, Any]:
    with _lock:
        job = _install_jobs.get(job_id)
        if not job:
            raise KeyError("instalação não encontrada")
        return dict(job)


def directory_size(path: Path) -> int:
    total = 0
    try:
        if not path.exists():
            return 0
        for entry in path.rglob("*"):
            if entry.is_file():
                try:
                    total += entry.stat().st_size
                except OSError:
                    pass
    except OSError:
        pass
    return total


def pipeline_cache_info(engines: dict[str, str] | None = None) -> dict[str, Any]:
    engines = engines or engine_paths()
    roots: list[Path] = [ROOT / "_PipelineCache"]
    if engines.get("kyty"):
        roots.append(Path(engines["kyty"]).parent / "_PipelineCache")
    if engines.get("shadps4"):
        shad_root = Path(engines["shadps4"]).parent
        roots += [shad_root / "shader_cache", shad_root / "cache", shad_root / "user" / "shader_cache"]
    unique: list[Path] = []
    seen: set[str] = set()
    for root in roots:
        key = str(root.resolve()) if root.exists() else str(root.absolute())
        if key not in seen:
            seen.add(key); unique.append(root)
    files = 0; size = 0
    for root in unique:
        try:
            for f in root.glob("*.bin"):
                if f.is_file():
                    files += 1; size += f.stat().st_size
        except OSError:
            pass
    return {"files": files, "size": size, "paths": [str(x) for x in unique]}


def memory_status() -> tuple[int, int]:
    if os.name == "nt":
        class MEMORYSTATUSEX(ctypes.Structure):
            _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong),
                        ("ullTotalPhys", ctypes.c_ulonglong), ("ullAvailPhys", ctypes.c_ulonglong),
                        ("ullTotalPageFile", ctypes.c_ulonglong), ("ullAvailPageFile", ctypes.c_ulonglong),
                        ("ullTotalVirtual", ctypes.c_ulonglong), ("ullAvailVirtual", ctypes.c_ulonglong),
                        ("sullAvailExtendedVirtual", ctypes.c_ulonglong)]
        state = MEMORYSTATUSEX(); state.dwLength = ctypes.sizeof(state)
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(state)):
            return int(state.ullTotalPhys), int(state.ullTotalPhys - state.ullAvailPhys)
    try:
        vals = {}
        for line in Path("/proc/meminfo").read_text().splitlines():
            key, val = line.split(":", 1); vals[key] = int(val.strip().split()[0]) * 1024
        total = vals.get("MemTotal", 0); avail = vals.get("MemAvailable", vals.get("MemFree", 0))
        return total, max(0, total - avail)
    except Exception:
        return 0, 0


def system_status() -> dict[str, Any]:
    total_ram, used_ram = memory_status()
    disk = shutil.disk_usage(ROOT)
    engines = engine_paths()
    cache = pipeline_cache_info(engines)
    vk = vulkan_probe()
    active = {gid: proc.pid for gid, proc in list(_processes.items()) if proc.poll() is None}
    return {
        "cpu_name": platform.processor() or platform.machine() or "CPU",
        "cpu_threads": os.cpu_count() or 1,
        "ram_total": total_ram, "ram_used": used_ram,
        "disk_total": disk.total, "disk_used": disk.used,
        "platform": platform.platform(), "python": platform.python_version(),
        "engines": engines, "core_ready": bool(engines["kyty"] or engines["sharpemu"] or engines["shadps4"]),
        "vulkan_runtime": bool(vk.get("available")), "vulkan": vk,
        "pipeline_cache": cache, "active_processes": active,
        "shadps4_cli": sorted(engine_cli_options(engines.get("shadps4", ""))),
        "time": int(time.time()),
    }


def game_by_id(game_id: str) -> tuple[list[dict[str, Any]], int, dict[str, Any]]:
    items = library()
    for i, game in enumerate(items):
        if game.get("id") == game_id:
            return items, i, game
    raise KeyError(game_id)


def _write_launch_log(log_path: Path, message: str) -> None:
    stamp = time.strftime("%H:%M:%S")
    line = f"[{stamp}] {message}"
    print(line, flush=True)
    with log_path.open("a", encoding="utf-8", errors="replace") as log:
        log.write(line + "\n")


def _pump_process_output(game_id: str, proc: subprocess.Popen[str], log_path: Path,
                         engine: str) -> None:
    if proc.stdout is None:
        return
    with log_path.open("a", encoding="utf-8", errors="replace") as log:
        for line in proc.stdout:
            rendered = f"[{engine}:{game_id}] {line.rstrip()}"
            print(rendered, flush=True)
            log.write(rendered + "\n")
            log.flush()


def _apply_cpu_affinity(proc: subprocess.Popen[str], cores: Any) -> str:
    if not isinstance(cores, list) or not cores:
        return "CPU affinity: sistema padrão"
    selected = sorted({core for core in cores if isinstance(core, int) and 0 <= core < 64})
    if not selected:
        return "CPU affinity: sistema padrão"
    mask = sum(1 << core for core in selected)
    try:
        if os.name == "nt":
            ok = ctypes.windll.kernel32.SetProcessAffinityMask(int(proc._handle), mask)
            if not ok:
                raise OSError(ctypes.get_last_error(), "SetProcessAffinityMask")
        elif hasattr(os, "sched_setaffinity"):
            os.sched_setaffinity(proc.pid, selected)
        else:
            return "CPU affinity: não suportada neste host"
    except OSError as exc:
        return f"CPU affinity falhou: {exc}"
    return f"CPU affinity: {','.join(str(core) for core in selected)}"


def _watch_process(game_id: str, proc: subprocess.Popen[str], started: float, engine: str,
                   log_path: Path) -> None:
    code = proc.wait()
    elapsed = max(0, int(time.time() - started))
    with _lock:
        _processes.pop(game_id, None)
        items = library()
        for i, game in enumerate(items):
            if game.get("id") != game_id:
                continue
            game["play_seconds"] = int(game.get("play_seconds", 0) or 0) + elapsed
            game["last_exit_code"] = int(code)
            game["last_log"] = str(log_path)
            game["status"] = (f"Encerrado normalmente ({engine})" if code == 0
                              else f"Engine encerrou com código {code} ({engine})")
            items[i] = game
            save_json(LIBRARY_FILE, items)
            break
    _write_launch_log(log_path, f"{engine} encerrou. Exit code={code}. Duração={elapsed}s")


def payload_bundle_error(path: Path) -> str:
    """Reject console-payload bundles that are not Kyty game executables."""
    if path.suffix.lower() != ".elf" or crispy_homebrew_bundle(path):
        return ""
    manifest = path.parent / "homebrew.js"
    if not manifest.is_file():
        return ""
    try:
        references_payload = path.name in manifest.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return ""
    if not references_payload:
        return ""
    return ("Payload homebrew detectado. Este ELF foi feito para PS5 Payload SDK, não para "
            "loader de jogo KytyPS5. Ele fecha com 'elf is not valid'. Use dump PS5 extraído "
            "com sce_sys/param.json e eboot válido.")


def crispy_homebrew_bundle(path: Path) -> bool:
    """Identify the bundled Crispy Doom PS5 SDK payload and its IWAD."""
    base = path if path.is_dir() else path.parent
    return ((base / "homebrew.js").is_file() and (base / "DOOM1.WAD").is_file()
            and ((base / "crispy-doom.elf").is_file() or path.suffix.lower() == ".elf"))

def launch_game(game_id: str) -> dict[str, Any]:
    items, index, game = game_by_id(game_id)
    p = Path(str(game.get("path", "")))
    if not p.exists():
        raise FileNotFoundError(str(p))
    engines = engine_paths(); cfg = settings()
    profile_override = str(game.get("profile_override", "global"))
    if profile_override in PROFILE_OVERRIDES:
        cfg.update(PROFILE_OVERRIDES[profile_override])
    platform_name = str(game.get("platform", "Auto")).upper()
    kind = str(game.get("kind", ""))
    if kind == "pkg":
        if platform_name != "PS4":
            raise RuntimeError("Somente PKG PS4 é aceito.")
        return start_pkg_install(game_id)
    if payload_error := payload_bundle_error(p):
        raise RuntimeError(payload_error)

    payload_args: list[str] = []
    input_profile: Path | None = None
    enable_mouse_capture = False
    if crispy_homebrew_bundle(p):
        base = p if p.is_dir() else p.parent
        p = p if p.is_file() else base / "crispy-doom.elf"
        payload_args = ["-iwad", "/app0/DOOM1.WAD"]

    if platform_name != "PS4" and not engines["kyty"] and engines["sharpemu"]:
        exe = engines["sharpemu"]
        target = p / "eboot.bin" if p.is_dir() else p
        if p.is_dir() and not target.is_file():
            found = list(p.rglob("eboot.bin"))[:1]
            if not found:
                raise RuntimeError("SharpEmu precisa de eboot.bin na pasta PS5.")
            target = found[0]
        args = [exe, str(target)]
        launch_target = str(target)
        engine = "SharpEmu (experimental)"
    elif platform_name == "PS4":
        exe = engines["shadps4"]
        if not exe:
            raise RuntimeError("Core PS4 não configurado. Configure um shadPS4.exe em Configurações.")
        input_profile = configure_ps4_keyboard_mouse(exe, cfg)
        title_id = normalize_title_id(game.get("title_id"))
        options = engine_cli_options(exe)
        boot_mode = str(game.get("ps4_boot_mode") or cfg.get("ps4_boot_mode") or "auto").lower()
        if boot_mode not in {"auto", "eboot", "cusa"}:
            boot_mode = "auto"
        # Auto favors direct ELF because it does not depend on shadPS4's own registered game-folder list.
        # CUSA mode is available for users who already keep the title in shadPS4's library.
        use_cusa = boot_mode == "cusa" and bool(CUSA_RE.fullmatch(title_id))
        if use_cusa:
            args = [exe, title_id]
            launch_target = title_id
        else:
            target = p / "eboot.bin" if p.is_dir() else p
            if p.is_dir() and not target.is_file():
                found = list(p.rglob("eboot.bin"))[:1]
                if not found:
                    raise RuntimeError("Nenhum eboot.bin encontrado na pasta PS4.")
                target = found[0]
            args = [exe, str(target)]
            launch_target = str(target)
        if bool(cfg.get("fullscreen", False)) and "--fullscreen" in options:
            args += ["--fullscreen", "true"]
        gpu = int(cfg.get("gpu_index", -1) or -1)
        if gpu >= 0:
            gpu_flag = next((flag for flag in ("--gpu-id", "--gpu", "--gpu_id") if flag in options), "")
            if gpu_flag:
                args += [gpu_flag, str(gpu)]
        present = str(cfg.get("present_mode", "Fifo"))
        present_flag = next((flag for flag in ("--present-mode", "--present_mode") if flag in options), "")
        if present_flag:
            args += [present_flag, present.lower()]
        engine = "PKG_DoW Core PS4"
    else:
        exe = engines["kyty"]
        if not exe:
            raise RuntimeError("Nenhum core PS5 encontrado. Compile Kyty ou instale SharpEmu em engines/ps5/sharpemu.")
        args = [exe, "--game", str(p)]
        keymap_args = kyty_keymap_args(cfg)
        args += keymap_args
        enable_mouse_capture = bool(keymap_args)
        for guest_arg in payload_args:
            args += ["--guest-arg", guest_arg]
        present = str(cfg.get("present_mode", "Fifo"))
        opt = str(cfg.get("shader_optimization", "Performance"))
        args += ["--present-mode", present, "--shader-optimization-type", opt]
        res = re.fullmatch(r"(\d+)x(\d+)", str(cfg.get("resolution", "1280x720")))
        if res:
            args += ["--screen-width", res.group(1), "--screen-height", res.group(2)]
        gpu = int(cfg.get("gpu_index", -1) or -1)
        if gpu >= 0:
            args += ["--gpu", str(gpu)]
        vblank = max(30, min(360, int(cfg.get("vblank_frequency", 60) or 60)))
        args += ["--vblank-frequency", str(vblank)]
        player = str(cfg.get("player_name", "Player1"))[:16] or "Player1"
        args += ["--user-name", player]
        if bool(cfg.get("fullscreen", False)):
            args += ["--fullscreen"]
        if os.name == "nt" and bool(cfg.get("redzone", True)):
            args += ["--redzone"]
        if bool(cfg.get("playgo_hack", False)):
            args += ["--playgo-hack"]
        bool_options = {
            "vulkan_validation": "--vulkan-validation",
            "gpu_assisted_validation": "--gpu-assisted-validation",
            "shader_validation": "--shader-validation",
            "graphics_debug_dump": "--graphics-debug-dump",
            "command_buffer_dump": "--command-buffer-dump",
            "readback_linear_images": "--readback-linear-images",
            "spirv_debug_printf": "--spirv-debug-printf",
        }
        for key, flag in bool_options.items():
            args += [flag, "true" if bool(cfg.get(key, False)) else "false"]
        shader_log_direction = str(cfg.get("shader_log_direction", "Console"))
        if shader_log_direction not in {"Silent", "Console", "File"}:
            shader_log_direction = "Console"
        printf_direction = str(cfg.get("printf_direction", "Console"))
        if printf_direction not in {"Silent", "Console", "File"}:
            printf_direction = "Console"
        shader_log_dir = LOGS / "shader"
        command_dump_dir = LOGS / "pm4"
        shader_log_dir.mkdir(exist_ok=True)
        command_dump_dir.mkdir(exist_ok=True)
        args += ["--shader-log-direction", shader_log_direction,
                 "--shader-log-folder", str(shader_log_dir),
                 "--command-buffer-dump-folder", str(command_dump_dir),
                 "--printf-direction", printf_direction]
        engine = "KytyPS5 Payload" if payload_args else "KytyPS5"

    log_path = LOGS / f"launch-{game_id}-{int(time.time())}.log"
    _write_launch_log(log_path, f"Launch {engine}: {subprocess.list2cmdline(args)}")
    flags = 0
    if os.name == "nt":
        flags = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
    started = time.time()
    try:
        launch_env = os.environ.copy()
        launch_env["PKG_DOW_TITLE_ID"] = normalize_title_id(game.get("title_id"))
        proc: subprocess.Popen[str] = subprocess.Popen(
            args, cwd=str(Path(exe).parent), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            creationflags=flags, env=launch_env, text=True, encoding="utf-8", errors="replace",
            bufsize=1)
    except Exception:
        _write_launch_log(log_path, "Launch falhou antes de criar processo.")
        raise
    _processes[game_id] = proc
    _write_launch_log(log_path, _apply_cpu_affinity(proc, cfg.get("host_cpu_affinity", [])))
    if input_profile:
        _write_launch_log(log_path, f"Entrada PKG_DoW: {input_profile}")
    window_title = f"PKG_DoW · {str(game.get('title') or 'Jogo')}"
    threading.Thread(
        target=brand_process_windows,
        args=(proc, window_title, enable_mouse_capture),
        daemon=True,
    ).start()
    threading.Thread(target=_pump_process_output, args=(game_id, proc, log_path, engine), daemon=True).start()
    threading.Thread(target=_watch_process, args=(game_id, proc, started, engine, log_path), daemon=True).start()
    game["last_played"] = int(started); game["status"] = f"Executando com {engine}"
    if platform_name == "PS4":
        game["last_boot_target"] = launch_target
    game["last_exit_code"] = None
    game["last_log"] = str(log_path)
    items[index] = game; save_json(LIBRARY_FILE, items)
    return {"ok": True, "pid": proc.pid, "engine": engine, "log": str(log_path), "args": args}


def open_path(path: Path) -> None:
    if os.name == "nt":
        os.startfile(str(path))  # type: ignore[attr-defined]
    elif sys.platform == "darwin":
        subprocess.Popen(["open", str(path)])
    else:
        subprocess.Popen(["xdg-open", str(path)])


def choose_import(kind: str) -> str:
    try:
        import tkinter as tk
        from tkinter import filedialog
        root = tk.Tk(); root.withdraw(); root.attributes("-topmost", True)
        if kind == "folder":
            value = filedialog.askdirectory(title="Importar pasta de jogo extraído")
        else:
            value = filedialog.askopenfilename(
                title="Importar jogo",
                filetypes=[("Jogos", "*.pkg *.bin *.elf"), ("PKG", "*.pkg"),
                           ("Executáveis", "*.bin *.elf"), ("Todos", "*.*")])
        root.destroy()
        return value or ""
    except Exception as tkinter_exc:
        # The official Windows embeddable Python runtime does not ship Tcl/Tk. Fall back to
        # WinForms through the PowerShell that already exists on normal Windows 10/11 installs.
        if os.name == "nt":
            if kind == "folder":
                script = (
                    "Add-Type -AssemblyName System.Windows.Forms; "
                    "$d=New-Object System.Windows.Forms.FolderBrowserDialog; "
                    "$d.Description='Importar pasta de jogo extraido'; "
                    "if($d.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK){"
                    "[Console]::Out.Write($d.SelectedPath)}"
                )
            else:
                script = (
                    "Add-Type -AssemblyName System.Windows.Forms; "
                    "$d=New-Object System.Windows.Forms.OpenFileDialog; "
                    "$d.Title='Importar jogo / PKG / ELF'; "
                    "$d.Filter='Jogos (*.pkg;*.elf;*.bin)|*.pkg;*.elf;*.bin|Todos (*.*)|*.*'; "
                    "if($d.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK){"
                    "[Console]::Out.Write($d.FileName)}"
                )
            try:
                completed = subprocess.run(
                    ["powershell.exe", "-NoLogo", "-NoProfile", "-STA", "-Command", script],
                    capture_output=True, text=True, timeout=300, check=False)
                if completed.returncode == 0:
                    return completed.stdout.strip()
            except (OSError, subprocess.SubprocessError):
                pass
        raise RuntimeError(f"Seletor nativo indisponível: {tkinter_exc}") from tkinter_exc


def json_bytes(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=False).encode("utf-8")


def browse_filesystem(raw_path: str = "") -> dict[str, Any]:
    """Small local file browser used inside PKG_DoW instead of native popup dialogs."""
    if not raw_path:
        if os.name == "nt":
            entries = []
            for letter in "ABCDEFGHIJKLMNOPQRSTUVWXYZ":
                drive = Path(f"{letter}:\\")
                if drive.exists():
                    entries.append({"name": f"Disco {letter}:", "path": str(drive), "type": "drive"})
            return {"path": "", "parent": "", "entries": entries}
        raw_path = "/"
    current = Path(raw_path).expanduser().resolve()
    if not current.is_dir():
        raise NotADirectoryError(str(current))
    entries = []
    try:
        children = sorted(current.iterdir(), key=lambda p: (not p.is_dir(), p.name.lower()))
    except PermissionError as exc:
        raise PermissionError(f"Sem acesso: {current}") from exc
    for child in children[:500]:
        try:
            if child.name.startswith(".") or not (child.is_dir() or child.suffix.lower() in {".pkg", ".elf", ".bin"}):
                continue
            entries.append({"name": child.name, "path": str(child),
                            "type": "folder" if child.is_dir() else "file",
                            "size": child.stat().st_size if child.is_file() else 0})
        except OSError:
            continue
    parent = str(current.parent) if current.parent != current else ""
    return {"path": str(current), "parent": parent, "entries": entries}


class Handler(BaseHTTPRequestHandler):
    server_version = "PKG_DoW/2.5"

    def log_message(self, fmt: str, *args: Any) -> None:
        # Keep the console useful instead of reenacting a 1990s access log waterfall.
        if self.path.startswith("/api/"):
            sys.stdout.write("[api] " + (fmt % args) + "\n")

    def send_json(self, value: Any, status: int = 200) -> None:
        body = json_bytes(value)
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers(); self.wfile.write(body)

    def error_json(self, exc: Exception, status: int = 400) -> None:
        self.send_json({"ok": False, "error": str(exc)}, status)

    def read_json(self) -> dict[str, Any]:
        length = int(self.headers.get("Content-Length", "0") or 0)
        if length > 2_000_000:
            raise ValueError("payload grande demais")
        raw = self.rfile.read(length) if length else b"{}"
        obj = json.loads(raw.decode("utf-8"))
        if not isinstance(obj, dict): raise ValueError("JSON precisa ser objeto")
        return obj

    def do_GET(self) -> None:
        try:
            u = urllib.parse.urlparse(self.path)
            if u.path == "/api/status": return self.send_json(system_status())
            if u.path == "/api/diagnostics":
                st = system_status()
                checks = [
                    {"name": "Python 3.10+", "ok": sys.version_info >= (3, 10), "detail": platform.python_version()},
                    {"name": "Runtime Vulkan", "ok": bool(st["vulkan_runtime"]), "detail": f"Loader {st.get('vulkan',{}).get('loader_version') or 'não detectado'}"},
                    {"name": "GPU Vulkan 1.3", "ok": any(int(g.get("api_version_raw",0)) >= ((1<<22)|(3<<12)) for g in st.get("vulkan",{}).get("gpus",[])), "detail": ", ".join(g.get("name","") for g in st.get("vulkan",{}).get("gpus",[])) or "nenhuma"},
                    {"name": "PS4 Vulkan extensions", "ok": any(bool(g.get("shadps4_ready")) for g in st.get("vulkan",{}).get("gpus",[])), "detail": "Vulkan 1.3 + VK_KHR_swapchain + VK_KHR_push_descriptor"},
                    {"name": "Auto tuning host GPU", "ok": bool(st.get("vulkan",{}).get("gpus",[])), "detail": ", ".join(f"{g.get('architecture','Vulkan')} / alvo VRAM {int(float(g.get('tuning',{}).get('target_ratio',0))*100)}%" for g in st.get("vulkan",{}).get("gpus",[])) or "nenhuma GPU"},
                    {"name": "Core PS5", "ok": bool(st["engines"].get("kyty")), "detail": st["engines"].get("kyty") or "não configurado"},
                    {"name": "Core PS4", "ok": bool(st["engines"].get("shadps4")), "detail": st["engines"].get("shadps4") or "opcional"},
                    {"name": "Extrator PKG interno", "ok": PKG_TOOL.is_file(), "detail": str(PKG_TOOL)},
                    {"name": "RAM >= 16 GB", "ok": int(st["ram_total"]) >= 16 * 1024**3, "detail": str(st["ram_total"])},
                    {"name": "Disco livre >= 10 GB", "ok": int(st["disk_total"] - st["disk_used"]) >= 10 * 1024**3, "detail": str(st["disk_total"] - st["disk_used"])},
                ]
                return self.send_json({"ok": True, "checks": checks, "status": st})
            if u.path == "/api/vulkan": return self.send_json(vulkan_probe(force=True))
            if u.path == "/api/cusa":
                games = [g for g in library() if bool(g.get("cusa")) or CUSA_RE.fullmatch(normalize_title_id(g.get("title_id")))]
                return self.send_json({"count": len(games), "games": games})
            if u.path == "/api/library": return self.send_json(library())
            if u.path == "/api/settings": return self.send_json(settings())
            if u.path == "/api/browse":
                q = urllib.parse.parse_qs(u.query)
                return self.send_json(browse_filesystem((q.get("path") or [""])[0]))
            if u.path == "/api/install-status":
                q = urllib.parse.parse_qs(u.query)
                return self.send_json(install_status((q.get("job") or [""])[0]))
            if u.path == "/api/logs":
                logs = sorted(LOGS.glob("*.log"), key=lambda p:p.stat().st_mtime, reverse=True)[:20]
                return self.send_json([{"name":p.name,"path":str(p),"size":p.stat().st_size,"mtime":int(p.stat().st_mtime)} for p in logs])
            if u.path == "/api/file":
                q = urllib.parse.parse_qs(u.query); raw=(q.get("path") or [""])[0]
                p=Path(raw)
                # Serve only the exact cover/icon paths already registered in the local library.
                # A localhost UI does not need an accidental "read any PNG on disk" endpoint.
                allowed = set()
                for game in library():
                    icon = str(game.get("icon") or "")
                    if not icon:
                        continue
                    icon_q = urllib.parse.parse_qs(urllib.parse.urlparse(icon).query)
                    icon_raw = (icon_q.get("path") or [""])[0]
                    if icon_raw:
                        try:
                            allowed.add(Path(icon_raw).resolve())
                        except OSError:
                            pass
                try:
                    resolved = p.resolve()
                except OSError:
                    return self.send_error(404)
                if resolved in allowed and resolved.is_file() and resolved.suffix.lower() in {".png",".jpg",".jpeg",".webp"}:
                    data=resolved.read_bytes(); self.send_response(200)
                    self.send_header("Content-Type", mimetypes.guess_type(resolved.name)[0] or "application/octet-stream")
                    self.send_header("Content-Length",str(len(data))); self.end_headers(); self.wfile.write(data); return
                return self.send_error(404)
            return self.serve_static(u.path)
        except Exception as exc:
            self.error_json(exc, 500)

    def do_POST(self) -> None:
        try:
            u=urllib.parse.urlparse(self.path); body=self.read_json()
            if u.path == "/api/import-path":
                item=upsert_game(describe_import(str(body.get("path", ""))))
                return self.send_json({"ok":True,"game":item})
            if u.path == "/api/install-pkg":
                return self.send_json(start_pkg_install(str(body.get("id", ""))))
            if u.path == "/api/import-dialog":
                raise RuntimeError("Diálogo externo desativado. Use navegador interno do PKG_DoW.")
            if u.path == "/api/scan-cusa":
                chosen = str(body.get("path") or "").strip()
                if not chosen: raise ValueError("Selecione pasta no navegador interno.")
                games = scan_cusa_root(chosen)
                return self.send_json({"ok": True, "path": chosen, "count": len(games), "games": games})
            if u.path == "/api/sync-ps4":
                games = sync_ps4_installs()
                return self.send_json({"ok": True, "path": str(PS4_GAMES_DIR),
                                       "count": len(games), "games": games})
            if u.path == "/api/launch": return self.send_json(launch_game(str(body.get("id",""))))
            if u.path == "/api/game-profile":
                game_id = str(body.get("id", "")); profile = str(body.get("profile", "global"))
                if profile not in {"global", *PROFILE_OVERRIDES.keys()}:
                    raise ValueError("perfil de jogo inválido")
                items = library(); game = next((g for g in items if g.get("id") == game_id), None)
                if game is None: raise KeyError("jogo não encontrado")
                game["profile_override"] = profile; save_json(LIBRARY_FILE, items)
                return self.send_json({"ok": True, "game": game})
            if u.path == "/api/game-boot-mode":
                game_id = str(body.get("id", "")); mode = str(body.get("mode", "auto")).lower()
                if mode not in {"auto", "eboot", "cusa"}: raise ValueError("modo de boot PS4 inválido")
                items,i,g = game_by_id(game_id); g["ps4_boot_mode"] = mode; items[i] = g; save_json(LIBRARY_FILE, items)
                return self.send_json({"ok": True, "game": g})
            if u.path == "/api/favorite":
                items,i,g=game_by_id(str(body.get("id",""))); g["favorite"]=bool(body.get("value",not g.get("favorite")))
                items[i]=g; save_json(LIBRARY_FILE,items); return self.send_json({"ok":True,"game":g})
            if u.path == "/api/remove":
                gid=str(body.get("id","")); items=[g for g in library() if g.get("id")!=gid]; save_json(LIBRARY_FILE,items)
                return self.send_json({"ok":True})
            if u.path == "/api/settings":
                cfg=settings()
                for k in DEFAULT_SETTINGS:
                    if k in body:
                        cfg[k]=body[k]
                if str(cfg.get("present_mode")) not in {"Fifo", "Mailbox", "Immediate"}:
                    raise ValueError("present mode inválido")
                if str(cfg.get("shader_optimization")) not in {"None", "Size", "Performance"}:
                    raise ValueError("shader optimization inválida")
                if str(cfg.get("performance_profile")) not in {"stable", "balanced", "turbo", "custom"}:
                    raise ValueError("perfil de performance inválido")
                if not re.fullmatch(r"\d{3,5}x\d{3,5}", str(cfg.get("resolution"))):
                    raise ValueError("resolução inválida")
                if str(cfg.get("ps4_boot_mode")) not in {"auto", "eboot", "cusa"}:
                    raise ValueError("modo PS4 inválido")
                if str(cfg.get("shader_log_direction")) not in {"Silent", "Console", "File"}:
                    raise ValueError("direção de log shader inválida")
                cfg["gpu_index"] = max(-1, int(cfg.get("gpu_index", -1) or -1))
                cfg["vblank_frequency"] = max(30, min(360, int(cfg.get("vblank_frequency", 60) or 60)))
                cfg["keyboard_mouse_enabled"] = bool(cfg.get("keyboard_mouse_enabled", True))
                cfg["mouse_sensitivity"] = max(0.2, min(4.0, float(cfg.get("mouse_sensitivity", 1.25) or 1.25)))
                cfg["player_name"] = str(cfg.get("player_name", "Player1"))[:16] or "Player1"
                cfg["fullscreen"] = bool(cfg.get("fullscreen", False))
                cfg["redzone"] = bool(cfg.get("redzone", True))
                cfg["playgo_hack"] = bool(cfg.get("playgo_hack", False))
                for key in ("vulkan_validation", "gpu_assisted_validation", "shader_validation",
                            "graphics_debug_dump", "command_buffer_dump", "readback_linear_images",
                            "spirv_debug_printf"):
                    cfg[key] = bool(cfg.get(key, False))
                affinity = cfg.get("host_cpu_affinity", [])
                if not isinstance(affinity, list) or any(not isinstance(core, int) or core < 0 or core > 63 for core in affinity):
                    raise ValueError("afinidade CPU inválida")
                cfg["host_cpu_affinity"] = sorted(set(affinity))
                save_json(SETTINGS_FILE,cfg); return self.send_json({"ok":True,"settings":cfg})
            if u.path == "/api/open-folder":
                raise RuntimeError("Abertura de janela externa desativada.")
            if u.path == "/api/open-engine":
                raise RuntimeError("Abertura de interface externa desativada.")
            if u.path == "/api/open-pipeline-cache":
                raise RuntimeError("Abertura de janela externa desativada.")
            if u.path == "/api/verify":
                checked=[]
                for g in library(): checked.append({"id":g.get("id"),"exists":Path(str(g.get("path",""))).exists()})
                return self.send_json({"ok":True,"checked":checked})
            if u.path == "/api/clean-cache":
                removed=0
                cache=DATA/"cache"
                if cache.exists():
                    for p in cache.rglob('*'):
                        if p.is_file(): removed+=p.stat().st_size
                    shutil.rmtree(cache,ignore_errors=True)
                cache.mkdir(exist_ok=True); return self.send_json({"ok":True,"removed":removed})
            return self.send_json({"ok":False,"error":"endpoint desconhecido"},404)
        except (KeyError, FileNotFoundError, ValueError, RuntimeError) as exc:
            self.error_json(exc,400)
        except Exception as exc:
            traceback.print_exc(); self.error_json(exc,500)

    def serve_static(self, url_path: str) -> None:
        rel = url_path.lstrip("/") or "index.html"
        p = (WEB / rel).resolve()
        if WEB.resolve() not in p.parents and p != WEB.resolve(): return self.send_error(403)
        if not p.is_file(): p = WEB / "index.html"
        data=p.read_bytes(); self.send_response(200)
        self.send_header("Content-Type",mimetypes.guess_type(p.name)[0] or "application/octet-stream")
        self.send_header("Content-Length",str(len(data)))
        self.send_header("Cache-Control","no-cache")
        self.end_headers(); self.wfile.write(data)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=48621)
    ap.add_argument("--no-browser", action="store_true")
    args = ap.parse_args()

    server = None
    selected_port = args.port
    last_error: OSError | None = None
    for candidate_port in range(args.port, args.port + 20):
        try:
            server = ThreadingHTTPServer((args.host, candidate_port), Handler)
            selected_port = candidate_port
            break
        except OSError as exc:
            last_error = exc
    if server is None:
        print(f"[PKG_DoW] Nenhuma porta livre entre {args.port} e {args.port + 19}: {last_error}")
        return 2

    url = f"http://{args.host}:{selected_port}/"
    print(f"PKG_DoW UX em {url}")
    print("Feche esta janela para encerrar o launcher.")
    if not args.no_browser:
        threading.Timer(.65, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0

if __name__ == "__main__": raise SystemExit(main())
