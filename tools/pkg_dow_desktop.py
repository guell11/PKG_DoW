#!/usr/bin/env python3
"""Minimal native shell for PKG_DoW web UI."""
from __future__ import annotations

import argparse
import queue
import re
import subprocess
import sys
import threading
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / "tools" / "pkg_dow_server.py"
URL_RE = re.compile(r"PKG_DoW UX em (http://[^\s]+)")


def require_qt() -> tuple[object, object]:
    try:
        from PyQt6.QtCore import QUrl
        from PyQt6.QtWidgets import QApplication, QMainWindow
        from PyQt6.QtWebEngineWidgets import QWebEngineView
    except ImportError as exc:
        print("PyQt6 ausente. Instale: py -3 -m pip install PyQt6 PyQt6-WebEngine", file=sys.stderr)
        raise SystemExit(3) from exc
    return (QUrl, QApplication, QMainWindow, QWebEngineView)


def stream_output(process: subprocess.Popen[str], lines: queue.Queue[str]) -> None:
    assert process.stdout is not None
    for line in process.stdout:
        text = line.rstrip()
        print(f"[web] {text}", flush=True)
        match = URL_RE.search(text)
        if match:
            lines.put(match.group(1))


def start_server(host: str, port: int) -> tuple[subprocess.Popen[str], str]:
    process = subprocess.Popen(
        [sys.executable, "-u", str(SERVER), "--host", host, "--port", str(port), "--no-browser"],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    urls: queue.Queue[str] = queue.Queue(maxsize=1)
    threading.Thread(target=stream_output, args=(process, urls), daemon=True).start()
    try:
        return process, urls.get(timeout=10)
    except queue.Empty:
        process.terminate()
        raise RuntimeError("Servidor local não iniciou. Ver terminal.")


def main() -> int:
    parser = argparse.ArgumentParser(description="PKG_DoW desktop shell")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", default=48621, type=int)
    args = parser.parse_args()
    QUrl, QApplication, QMainWindow, QWebEngineView = require_qt()
    try:
        server, url = start_server(args.host, args.port)
    except (OSError, RuntimeError) as exc:
        print(f"Falha desktop shell: {exc}", file=sys.stderr)
        return 2

    app = QApplication(sys.argv)
    window = QMainWindow()
    view = QWebEngineView(window)
    window.setCentralWidget(view)
    window.setWindowTitle("PKG_DoW")
    window.resize(1440, 900)
    window.setMinimumSize(1024, 680)
    view.load(QUrl(url))
    window.show()
    try:
        return app.exec()
    finally:
        if server.poll() is None:
            server.terminate()
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()


if __name__ == "__main__":
    raise SystemExit(main())
