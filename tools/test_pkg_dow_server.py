#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import json
import os
import struct
import tempfile
import threading
import urllib.request
from http.server import ThreadingHTTPServer
from pathlib import Path

SERVER = Path(__file__).with_name("pkg_dow_server.py")
spec = importlib.util.spec_from_file_location("pkg_dow_server", SERVER)
assert spec and spec.loader
server = importlib.util.module_from_spec(spec)
spec.loader.exec_module(server)


def build_sfo() -> bytes:
    keys = b"TITLE\0ATTRIBUTE\0TITLE_ID\0"
    title = b"Fixture Game\0"
    title_slot = title + b"\0" * (32 - len(title))
    integer = struct.pack("<I", 0x12345678)
    title_id = b"PPSA99999\0"
    title_id_slot = title_id + b"\0" * (16 - len(title_id))
    data = title_slot + integer + title_id_slot
    key_off = 20 + 3 * 16
    data_off = key_off + len(keys)
    blob = bytearray(b"\x00PSF" + struct.pack("<IIII", 0x101, key_off, data_off, 3))
    blob += struct.pack("<HHIII", 0, 0x0204, len(title), 32, 0)
    blob += struct.pack("<HHIII", 6, 0x0404, 4, 4, 32)
    blob += struct.pack("<HHIII", 16, 0x0204, len(title_id), 16, 36)
    blob += keys + data
    return bytes(blob)


def build_pkg(title_id: str = "LAPY20014") -> bytes:
    content_id = f"VR1234-{title_id}_00-0000000000000000".encode("ascii")
    table_offset = 0x5A0
    blob = bytearray(table_offset + 0x20)
    blob[:4] = b"\x7fCNT"
    struct.pack_into(">I", blob, 0x10, 1)
    struct.pack_into(">H", blob, 0x16, 1)
    struct.pack_into(">I", blob, 0x18, table_offset)
    blob[0x40:0x40 + len(content_id)] = content_id
    struct.pack_into(">I", blob, 0x70, 15)
    struct.pack_into(">I", blob, 0x74, 26)
    struct.pack_into(">Q", blob, 0x418, 16 * 1024 * 1024)
    struct.pack_into(">I", blob, table_offset, 0x20)
    struct.pack_into(">I", blob, table_offset + 8, 0x80000000)
    return bytes(blob)


def request_json(url: str, method: str = "GET", body: dict | None = None) -> tuple[int, dict]:
    data = json.dumps(body).encode("utf-8") if body is not None else None
    request = urllib.request.Request(
        url, data=data, method=method,
        headers={"Content-Type": "application/json"} if data is not None else {})
    with urllib.request.urlopen(request, timeout=5) as response:
        return response.status, json.loads(response.read().decode("utf-8"))


def smoke_http(base: Path, import_path: Path) -> None:
    """Exercise the public local HTTP API without touching the user's library."""
    old_data, old_logs, old_ps4_games = server.DATA, server.LOGS, server.PS4_GAMES_DIR
    old_library, old_settings = server.LIBRARY_FILE, server.SETTINGS_FILE
    server.DATA = base / "userdata"
    server.LOGS = base / "logs"
    server.LIBRARY_FILE = server.DATA / "library.json"
    server.SETTINGS_FILE = server.DATA / "settings.json"
    server.PS4_GAMES_DIR = server.DATA / "ps4-games"
    server.DATA.mkdir()
    server.LOGS.mkdir()
    httpd = ThreadingHTTPServer(("127.0.0.1", 0), server.Handler)
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    url = f"http://127.0.0.1:{httpd.server_address[1]}"
    try:
        for route in ("/", "/library.html", "/import.html", "/settings.html",
                      "/system.html", "/app.js", "/styles.css"):
            with urllib.request.urlopen(url + route, timeout=5) as response:
                assert response.status == 200, route
                assert response.read(), route

        status, payload = request_json(url + "/api/status")
        assert status == 200 and "engines" in payload, payload
        assert payload["engines"]["pkg_installer"] == "", payload
        status, payload = request_json(url + "/api/settings", "POST", {
            "performance_profile": "turbo", "present_mode": "Mailbox"})
        assert status == 200 and payload["settings"]["performance_profile"] == "turbo", payload
        status, payload = request_json(url + "/api/import-path", "POST", {"path": str(import_path)})
        assert status == 200 and payload["game"]["cusa"], payload
        status, payload = request_json(url + "/api/library")
        assert status == 200 and len(payload) == 1, payload
        status, payload = request_json(url + "/api/verify", "POST", {})
        assert status == 200 and payload["checked"][0]["exists"], payload
    finally:
        httpd.shutdown()
        thread.join(timeout=5)
        httpd.server_close()
        server.DATA, server.LOGS, server.PS4_GAMES_DIR = old_data, old_logs, old_ps4_games
        server.LIBRARY_FILE, server.SETTINGS_FILE = old_library, old_settings


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        sfo = base / "param.sfo"
        sfo.write_bytes(build_sfo())
        parsed = server.parse_sfo(sfo)
        assert parsed["TITLE"] == "Fixture Game", parsed
        assert parsed["ATTRIBUTE"] == 0x12345678, parsed
        assert parsed["TITLE_ID"] == "PPSA99999", parsed
        assert server.infer_platform(base, parsed) == "PS5"

        pkg = base / "sample.pkg"
        pkg.write_bytes(build_pkg())
        meta = server.pkg_metadata(pkg)
        assert meta.get("content_id") == "VR1234-LAPY20014_00-0000000000000000", meta
        assert meta.get("title_id") == "LAPY20014", meta
        assert meta.get("pkg_valid") and meta.get("pkg_encrypted"), meta
        assert not meta.get("pkg_direct_extractable"), meta
        assert server.infer_platform(pkg, meta) == "PS4", meta
        item = server.describe_import(str(pkg))
        assert item["kind"] == "pkg" and item["platform"] == "PS4", item
        assert item["status"] == "Pronto para instalar", item
        assert item["pkg_entries"] == 1 and item["pkg_pfs_size"] == 16 * 1024 * 1024, item

        # Homebrew title IDs keep stable identities after package installation/moves.
        assert server.game_identity(base / "one", "QUIG00001") == server.game_identity(base / "two", "QUIG00001")

        # Optional PKG helper is downloaded separately and must not be required by clean checkout.
        fake_core = base / "core" / "PKG_DoW-CorePS4.exe"
        fake_core.parent.mkdir()
        fake_core.write_bytes(b"")
        profile = server.configure_ps4_keyboard_mouse(str(fake_core), {
            "keyboard_mouse_enabled": True, "mouse_sensitivity": 1.5})
        profile_text = profile.read_text(encoding="utf-8")
        assert "axis_left_y_minus = w" in profile_text, profile_text
        assert "axis_left_x_plus = d" in profile_text, profile_text
        assert "mouse_to_joystick = right" in profile_text, profile_text
        assert "r2 = leftbutton" in profile_text and "l2 = rightbutton" in profile_text
        server.configure_ps4_keyboard_mouse(str(fake_core), {
            "keyboard_mouse_enabled": True, "mouse_sensitivity": 1.5})
        assert profile.read_text(encoding="utf-8").count(server.PKG_DOW_INPUT_BEGIN) == 1
        core_config = server.load_json(fake_core.parent / "user" / "config.json", {})
        assert core_config["Input"]["use_unified_input_config"] is True
        assert core_config["Input"]["use_mice_as_mice"] is False
        if os.name == "nt":
            assert server.set_process_window_titles(0x7FFFFFFF, "PKG_DoW") == 0
            assert server.send_process_key(0x7FFFFFFF, 0x76) == 0
        keymap = server.kyty_keymap_args({
            "keyboard_mouse_enabled": True, "mouse_sensitivity": 1.5})
        assert "LeftStickUp=W" in keymap and "LeftStickRight=D" in keymap
        assert "R2=Mouse:Left" in keymap and "L2=Mouse:Right" in keymap
        assert "MouseSensitivity=1.50" in keymap
        assert not server.kyty_keymap_args({"keyboard_mouse_enabled": False})
        root_view = server.browse_filesystem(str(base))
        assert root_view["path"] == str(base.resolve()), root_view
        assert any(entry["name"] == "sample.pkg" and entry["type"] == "file"
                   for entry in root_view["entries"]), root_view

        # CUSA folder integration: title ID fallback, registry scan and per-game metadata.
        cusa_root = base / "games"
        game_dir = cusa_root / "CUSA01234"
        (game_dir / "sce_sys").mkdir(parents=True)
        cusa_sfo = build_sfo().replace(b"PPSA99999", b"CUSA01234")
        (game_dir / "sce_sys" / "param.sfo").write_bytes(cusa_sfo)
        (game_dir / "eboot.bin").write_bytes(b"ELF")
        old_library = server.LIBRARY_FILE
        server.LIBRARY_FILE = base / "library.json"
        try:
            legacy = dict(item, id="legacy-path-id", favorite=True, added_at=1)
            server.save_json(server.LIBRARY_FILE, [legacy, item])
            merged = server.upsert_game(dict(item))
            assert len(server.library()) == 1 and merged["favorite"] and merged["added_at"] == 1
            server.save_json(server.LIBRARY_FILE, [])

            cusa_item = server.describe_import(str(game_dir))
            assert cusa_item["title_id"] == "CUSA01234", cusa_item
            assert cusa_item["platform"] == "PS4" and cusa_item["cusa"], cusa_item
            scanned = server.scan_cusa_root(str(cusa_root))
            assert len(scanned) == 1 and scanned[0]["title_id"] == "CUSA01234", scanned
            assert scanned[0]["id"] == cusa_item["id"], (scanned[0], cusa_item)
            moved = base / "other" / "CUSA01234"
            (moved / "sce_sys").mkdir(parents=True)
            (moved / "sce_sys" / "param.sfo").write_bytes(cusa_sfo)
            (moved / "eboot.bin").write_bytes(b"ELF")
            moved_item = server.describe_import(str(moved))
            assert moved_item["id"] == cusa_item["id"], (moved_item, cusa_item)
        finally:
            server.LIBRARY_FILE = old_library

        # CLI capability discovery follows the installed shadPS4 build instead of guessing flags.
        # The fixture needs to be executable on the host that is running this test.
        fake = base / ("fake_shad.cmd" if os.name == "nt" else "fake_shad")
        if os.name == "nt":
            fake.write_text("@echo off\r\necho Usage: shadPS4 --fullscreen --gpu-id --present-mode -g\r\n", encoding="utf-8")
        else:
            fake.write_text("#!/bin/sh\necho 'Usage: shadPS4 --fullscreen --gpu-id --present-mode -g'\n", encoding="utf-8")
            fake.chmod(0o755)
        opts = server.engine_cli_options(str(fake))
        assert {"--fullscreen", "--gpu-id", "--present-mode", "-g"}.issubset(opts), opts

        # Host-GPU policy is automatic and architecture-aware, but deliberately stops at Vulkan.
        assert server.classify_host_gpu("NVIDIA GeForce RTX 3060", 0x10DE) == "NVIDIA Ampere"
        assert server.classify_host_gpu("NVIDIA GeForce RTX 4060", 0x10DE) == "NVIDIA Ada Lovelace"
        assert server.classify_host_gpu("NVIDIA GeForce RTX 5060", 0x10DE) == "NVIDIA Blackwell"
        p8 = server.adaptive_vram_policy(8 * 1024**3, "Dedicada")
        assert 0.84 <= p8["target_ratio"] <= 0.88 and p8["target"] < 8 * 1024**3, p8
        p12 = server.adaptive_vram_policy(12 * 1024**3, "Dedicada")
        assert p12["target_ratio"] == 0.90 and p12["target"] > p8["target"], (p8, p12)
        shared = server.adaptive_vram_policy(16 * 1024**3, "Integrada")
        assert shared["target_ratio"] == 0.70, shared

        smoke_http(base, game_dir)

    print("PKG_DoW server/API tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
