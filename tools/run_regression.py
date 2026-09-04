#!/usr/bin/env python3
"""Reproducible regression entry point for the local launcher and emulator core.

It always runs the zero-dependency Python smoke tests. If a configured
CMake build exists, it also builds the C++ regression targets and executes CTest.  A
missing build is an explicit SKIP by default so a checkout without proprietary game data
or a C++ toolchain remains testable; use --require-core in CI to turn that into an error.
"""
from __future__ import annotations

import argparse
import datetime as dt
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import TextIO

ROOT = Path(__file__).resolve().parents[1]
PYTHON_SOURCES = (
    ROOT / "tools" / "pkg_dow_server.py",
    ROOT / "tools" / "test_pkg_dow_server.py",
    ROOT / "tools" / "run_regression.py",
)


class Reporter:
    def __init__(self, log: TextIO) -> None:
        self.log = log
        self.passed: list[str] = []
        self.failed: list[str] = []
        self.skipped: list[str] = []

    def write(self, line: str = "") -> None:
        print(line, flush=True)
        self.log.write(line + "\n")
        self.log.flush()

    def run(self, name: str, command: list[str], cwd: Path = ROOT) -> bool:
        self.write(f"\n[RUN ] {name}")
        self.write("[CMD ] " + subprocess.list2cmdline(command))
        try:
            process = subprocess.Popen(command, cwd=cwd, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, text=True,
                                       encoding="utf-8", errors="replace")
        except OSError as exc:
            self.failed.append(name)
            self.write(f"[FAIL] {name}: could not start ({exc})")
            return False
        assert process.stdout is not None
        for line in process.stdout:
            self.write("       " + line.rstrip())
        code = process.wait()
        if code == 0:
            self.passed.append(name)
            self.write(f"[PASS] {name}")
            return True
        self.failed.append(name)
        self.write(f"[FAIL] {name}: exit code {code}")
        return False

    def skip(self, name: str, reason: str) -> None:
        self.skipped.append(name)
        self.write(f"[SKIP] {name}: {reason}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run launcher and optional CTest regressions.")
    parser.add_argument("--build-dir", default=os.environ.get("KYTY_BUILD_DIR", "_Build/windows"),
                        help="configured CMake build directory (default: _Build/windows)")
    parser.add_argument("--skip-core", action="store_true",
                        help="do not build or run CTest even if the build directory exists")
    parser.add_argument("--require-core", action="store_true",
                        help="return failure if the configured CMake build is unavailable")
    parser.add_argument("--log", type=Path, help="log destination (defaults to logs/regression-<UTC>.log)")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if sys.version_info < (3, 10):
        print("[FAIL] Python 3.10+ is required for the regression harness.", file=sys.stderr)
        return 2

    build_dir = Path(args.build_dir)
    if not build_dir.is_absolute():
        build_dir = ROOT / build_dir
    timestamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    log_path = args.log or ROOT / "logs" / f"regression-{timestamp}.log"
    if not log_path.is_absolute():
        log_path = ROOT / log_path
    log_path.parent.mkdir(parents=True, exist_ok=True)

    with log_path.open("w", encoding="utf-8", newline="\n") as log:
        report = Reporter(log)
        report.write("KytyPS5 regression smoke")
        report.write(f"Repository: {ROOT}")
        report.write(f"Python: {sys.version.split()[0]}")
        report.write(f"Log: {log_path}")

        syntax_check = (
            "from pathlib import Path; import sys; "
            "[compile(Path(path).read_text(encoding='utf-8'), path, 'exec') for path in sys.argv[1:]]; "
            "print('Python syntax: PASS')"
        )
        report.run("Python syntax", [sys.executable, "-B", "-c", syntax_check,
                                     *(str(path) for path in PYTHON_SOURCES)])
        report.run("PKG_DoW launcher/API", [sys.executable, "-B", "tools/test_pkg_dow_server.py"])

        core_available = (build_dir / "CMakeCache.txt").is_file()
        if args.skip_core:
            report.skip("C++ core / CTest", "disabled with --skip-core")
        elif not core_available:
            report.skip("C++ core / CTest", f"no configured build at {build_dir}")
        elif shutil.which("cmake") is None:
            report.failed.append("C++ core / CTest")
            report.write("[FAIL] C++ core / CTest: cmake is not available on PATH")
        else:
            report.run("Build C++ regression targets", ["cmake", "--build", str(build_dir),
                                                           "--target", "kyty_tests", "kyty_emulator", "--parallel"])
            report.run("CTest", ["ctest", "--test-dir", str(build_dir), "--output-on-failure", "--no-tests=error"])

        report.write("\nSummary")
        report.write(f"  PASS: {len(report.passed)}")
        report.write(f"  FAIL: {len(report.failed)}")
        report.write(f"  SKIP: {len(report.skipped)}")
        if report.skipped:
            report.write("  Skipped: " + ", ".join(report.skipped))
        report.write("  FPS: not measured (no runnable host core, Vulkan execution path, or legal game content configured).")
        if report.failed:
            return 1
        if args.require_core and "C++ core / CTest" in report.skipped:
            report.write("[FAIL] --require-core was requested, but C++ core / CTest was skipped.")
            return 2
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
