#!/usr/bin/env python3
"""Windows runtime doctor for integrated emulator engines.

Designed to catch loader failures *before* a game launch. In particular, Windows
STATUS_INVALID_IMAGE_FORMAT (0xC000007B) is a process-loader error, not a shader
or Vulkan render failure. The doctor validates PE architecture, imported DLLs,
common local-DLL shadowing problems and can quarantine wrong-architecture local
runtime DLLs without deleting them.
"""
from __future__ import annotations

import argparse
import ctypes
import json
import os
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path
from typing import Iterable

MACHINE_NAMES = {
    0x014C: "x86",
    0x8664: "x64/AMD64",
    0xAA64: "ARM64",
    0x01C4: "ARMv7",
}
EXPECTED_MACHINE = 0x8664
STATUS_NAMES = {
    0xC0000005: "STATUS_ACCESS_VIOLATION",
    0xC000001D: "STATUS_ILLEGAL_INSTRUCTION",
    0xC000007B: "STATUS_INVALID_IMAGE_FORMAT",
    0xC0000094: "STATUS_INTEGER_DIVIDE_BY_ZERO",
    0xC00000FD: "STATUS_STACK_OVERFLOW",
    0xC0000135: "STATUS_DLL_NOT_FOUND",
    0xC0000142: "STATUS_DLL_INIT_FAILED",
    0xC0000409: "STATUS_STACK_BUFFER_OVERRUN",
}


def u32(code: int) -> int:
    return int(code) & 0xFFFFFFFF


def status_name(code: int) -> str:
    value = u32(code)
    return STATUS_NAMES.get(value, f"NTSTATUS/EXIT 0x{value:08X}")


def pe_info(path: Path) -> dict:
    """Parse just enough PE/COFF to identify architecture and imports."""
    out = {"path": str(path), "is_pe": False, "machine": None, "machine_name": "unknown", "imports": [], "error": ""}
    try:
        data = path.read_bytes()
        if len(data) < 0x40 or data[:2] != b"MZ":
            out["error"] = "arquivo não possui cabeçalho MZ"
            return out
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if pe + 24 > len(data) or data[pe:pe+4] != b"PE\0\0":
            out["error"] = "assinatura PE inválida"
            return out
        machine, sections, _, _, _, opt_size, _ = struct.unpack_from("<HHIIIHH", data, pe + 4)
        out["is_pe"] = True
        out["machine"] = machine
        out["machine_name"] = MACHINE_NAMES.get(machine, f"0x{machine:04X}")
        opt = pe + 24
        if opt + opt_size > len(data) or opt_size < 96:
            return out
        magic = struct.unpack_from("<H", data, opt)[0]
        if magic == 0x20B:  # PE32+
            dir_off = opt + 112
        elif magic == 0x10B:  # PE32
            dir_off = opt + 96
        else:
            return out
        if dir_off + 16 > opt + opt_size:
            return out
        import_rva, import_size = struct.unpack_from("<II", data, dir_off + 8)
        sec_off = opt + opt_size
        sec_rows = []
        for i in range(sections):
            s = sec_off + 40 * i
            if s + 40 > len(data):
                break
            vsize, va, raw_size, raw = struct.unpack_from("<IIII", data, s + 8)
            sec_rows.append((va, max(vsize, raw_size), raw, raw_size))

        def rva_to_off(rva: int) -> int | None:
            for va, size, raw, raw_size in sec_rows:
                if va <= rva < va + size:
                    delta = rva - va
                    if delta < raw_size:
                        return raw + delta
            return None

        if import_rva:
            cur = rva_to_off(import_rva)
            if cur is not None:
                imports = []
                for _ in range(4096):
                    if cur + 20 > len(data):
                        break
                    row = struct.unpack_from("<IIIII", data, cur)
                    if not any(row):
                        break
                    name_rva = row[3]
                    noff = rva_to_off(name_rva)
                    if noff is not None and noff < len(data):
                        end = data.find(b"\0", noff, min(len(data), noff + 512))
                        if end < 0:
                            end = min(len(data), noff + 512)
                        try:
                            name = data[noff:end].decode("ascii", "replace")
                            if name:
                                imports.append(name)
                        except Exception:
                            pass
                    cur += 20
                out["imports"] = sorted(set(imports), key=str.lower)
    except Exception as exc:
        out["error"] = str(exc)
    return out


def candidate_dll_paths(name: str, exe_dir: Path) -> Iterable[Path]:
    seen: set[str] = set()
    roots = [exe_dir]
    if os.name == "nt":
        windir = Path(os.environ.get("WINDIR", r"C:\Windows"))
        roots += [windir / "System32"]
    for entry in os.environ.get("PATH", "").split(os.pathsep):
        if entry:
            roots.append(Path(entry))
    for root in roots:
        p = root / name
        key = str(p).lower()
        if key not in seen:
            seen.add(key)
            yield p


def resolve_import(name: str, exe_dir: Path) -> Path | None:
    low = name.lower()
    # API-set DLL names are virtual contracts resolved by the Windows loader.
    if low.startswith("api-ms-win-") or low.startswith("ext-ms-win-"):
        return None
    for p in candidate_dll_paths(name, exe_dir):
        if p.is_file():
            return p
    return None


def quarantine_wrong_local_dlls(exe: Path, imports: list[str], do_fix: bool) -> list[dict]:
    issues = []
    qdir = exe.parent / "_quarantine_wrong_arch"
    for name in imports:
        local = exe.parent / name
        if not local.is_file():
            continue
        info = pe_info(local)
        machine = info.get("machine")
        if machine and machine != EXPECTED_MACHINE:
            action = "detected"
            target = None
            if do_fix:
                qdir.mkdir(parents=True, exist_ok=True)
                target = qdir / f"{local.name}.{int(time.time())}.disabled"
                try:
                    shutil.move(str(local), str(target))
                    action = "quarantined"
                except Exception as exc:
                    action = f"quarantine_failed: {exc}"
            issues.append({
                "dll": name,
                "path": str(local),
                "machine": info.get("machine_name"),
                "action": action,
                "target": str(target) if target else "",
            })
    return issues


def probe_engine(exe: Path, timeout: int = 8) -> dict:
    out = {"ran": False, "returncode": None, "status": "", "stdout": "", "error": ""}
    if os.name != "nt":
        out["error"] = "probe de execução só é válido no Windows"
        return out
    try:
        p = subprocess.run([str(exe), "--help"], cwd=str(exe.parent), capture_output=True,
                           text=True, errors="replace", timeout=timeout, check=False)
        out["ran"] = True
        out["returncode"] = int(p.returncode)
        out["status"] = status_name(p.returncode)
        out["stdout"] = ((p.stdout or "") + (p.stderr or ""))[-12000:]
    except subprocess.TimeoutExpired as exc:
        # If --help stays alive, the Windows loader definitely succeeded.
        out["ran"] = True
        out["returncode"] = 0
        out["status"] = "loader OK (process survived probe timeout)"
        out["stdout"] = ((exc.stdout or "") if isinstance(exc.stdout, str) else "")[-12000:]
    except OSError as exc:
        out["error"] = f"WinError={getattr(exc, 'winerror', None)} {exc}"
        out["returncode"] = -int(getattr(exc, "winerror", 1) or 1)
        out["status"] = out["error"]
    except Exception as exc:
        out["error"] = str(exc)
    return out


def check_common_runtime() -> dict:
    result = {"vulkan": False, "vcruntime140": False, "vcruntime140_1": False, "msvcp140": False}
    if os.name != "nt":
        return result
    for key, dll in (("vulkan", "vulkan-1.dll"), ("vcruntime140", "vcruntime140.dll"),
                     ("vcruntime140_1", "vcruntime140_1.dll"), ("msvcp140", "msvcp140.dll")):
        try:
            ctypes.WinDLL(dll)
            result[key] = True
        except OSError:
            result[key] = False
    return result


def inspect(exe: Path, do_fix: bool, do_probe: bool) -> dict:
    report = {
        "engine": str(exe), "exists": exe.is_file(), "pe": {}, "runtime": check_common_runtime(),
        "imports": [], "resolved_imports": [], "wrong_local_dlls": [], "probe": {}, "fatal": [], "warnings": [],
    }
    if not exe.is_file():
        report["fatal"].append("engine executable not found")
        return report
    pe = pe_info(exe)
    report["pe"] = pe
    if not pe.get("is_pe"):
        report["fatal"].append("kyty_emulator.exe não é um executável PE Windows válido")
        return report
    if pe.get("machine") != EXPECTED_MACHINE:
        report["fatal"].append(f"engine tem arquitetura {pe.get('machine_name')}; necessário x64/AMD64")
    imports = list(pe.get("imports") or [])
    report["imports"] = imports
    wrong = quarantine_wrong_local_dlls(exe, imports, do_fix)
    report["wrong_local_dlls"] = wrong
    if wrong and not do_fix:
        report["fatal"].append("há DLL(s) locais de arquitetura incompatível ao lado da engine")
    elif wrong and do_fix:
        report["warnings"].append("DLL(s) locais incompatíveis foram movidas para _quarantine_wrong_arch")

    # Re-resolve after possible quarantine.
    resolved = []
    for name in imports:
        p = resolve_import(name, exe.parent)
        row = {"name": name, "path": str(p) if p else "", "machine": "", "ok": True}
        low = name.lower()
        if low.startswith("api-ms-win-") or low.startswith("ext-ms-win-"):
            row["path"] = "Windows API-set"
        elif p is None:
            row["ok"] = False
            report["warnings"].append(f"dependência não localizada no scan: {name}")
        else:
            pi = pe_info(p)
            if pi.get("is_pe"):
                row["machine"] = pi.get("machine_name", "")
                if pi.get("machine") not in (None, EXPECTED_MACHINE):
                    row["ok"] = False
                    report["fatal"].append(f"DLL {name} resolve para {row['machine']} em {p}")
        resolved.append(row)
    report["resolved_imports"] = resolved

    if do_probe and not report["fatal"]:
        probe = probe_engine(exe)
        report["probe"] = probe
        rc = probe.get("returncode")
        if rc is not None and u32(int(rc)) == 0xC000007B:
            report["fatal"].append("0xC000007B: Windows recusou a imagem da engine ou uma DLL dependente por arquitetura/formato incompatível")
        elif rc is not None and u32(int(rc)) == 0xC0000135:
            report["fatal"].append("0xC0000135: uma DLL exigida pela engine está ausente")
        elif probe.get("error"):
            report["fatal"].append(str(probe["error"]))
    return report


def print_human(report: dict) -> None:
    prefix = "[ENGINE-DOCTOR]"
    print(f"{prefix} engine={report.get('engine')}", flush=True)
    if not report.get("exists"):
        print(f"{prefix} AUSENTE", flush=True); return
    pe = report.get("pe") or {}
    print(f"{prefix} PE={pe.get('machine_name','unknown')} imports={len(report.get('imports') or [])}", flush=True)
    runtime = report.get("runtime") or {}
    print(f"{prefix} runtime Vulkan={'OK' if runtime.get('vulkan') else 'FALTA'} | VC140={'OK' if runtime.get('vcruntime140') else 'FALTA'} | VC140_1={'OK' if runtime.get('vcruntime140_1') else 'FALTA'} | MSVCP140={'OK' if runtime.get('msvcp140') else 'FALTA'}", flush=True)
    for item in report.get("wrong_local_dlls") or []:
        print(f"{prefix} DLL incompatível: {item['dll']} [{item['machine']}] action={item['action']}", flush=True)
    probe = report.get("probe") or {}
    if probe:
        print(f"{prefix} probe --help: rc={probe.get('returncode')} {probe.get('status','')}", flush=True)
    for w in report.get("warnings") or []:
        print(f"{prefix} WARN: {w}", flush=True)
    for e in report.get("fatal") or []:
        print(f"{prefix} FATAL: {e}", flush=True)
    if not report.get("fatal"):
        print(f"{prefix} runtime da engine parece carregável.", flush=True)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine", required=True)
    ap.add_argument("--fix", action="store_true", help="quarentena DLLs locais de arquitetura incorreta")
    ap.add_argument("--probe", action="store_true", help="executa engine --help para validar o Windows loader")
    ap.add_argument("--json", action="store_true")
    ns = ap.parse_args()
    report = inspect(Path(ns.engine).resolve(), ns.fix, ns.probe)
    if ns.json:
        print(json.dumps(report, ensure_ascii=False, indent=2))
    else:
        print_human(report)
    return 2 if report.get("fatal") else 0


if __name__ == "__main__":
    raise SystemExit(main())
