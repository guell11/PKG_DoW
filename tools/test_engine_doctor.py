#!/usr/bin/env python3
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
import engine_doctor as d

# Use setuptools' tiny Windows launcher stubs when available; otherwise only status tests run.
roots = [Path('/opt/pyvenv/lib/python3.13/site-packages/setuptools'), Path(sys.prefix) / 'Lib/site-packages/setuptools']
for root in roots:
    p64 = root / 'cli-64.exe'
    p32 = root / 'cli-32.exe'
    if p64.exists() and p32.exists():
        assert d.pe_info(p64)['machine'] == 0x8664
        assert d.pe_info(p32)['machine'] == 0x014C
        assert 'KERNEL32.dll' in d.pe_info(p64)['imports']
        break
assert d.u32(-1073741701) == 0xC000007B
assert d.status_name(-1073741701) == 'STATUS_INVALID_IMAGE_FORMAT'
assert d.status_name(-1073741819) == 'STATUS_ACCESS_VIOLATION'
print('engine_doctor tests: PASS')
