# Validation 2.4.1

Executado neste ambiente:

- `python -m py_compile tools/engine_doctor.py`: PASS
- `python -m py_compile tools/pkg_dow_qt.py`: PASS
- `python tools/test_engine_doctor.py`: PASS
- `python tools/test_pkg_dow_server.py`: PASS
- PE parser verificado contra stubs Windows x86, AMD64 e ARM64 do setuptools: PASS
- Mapeamento `-1073741701 -> 0xC000007B -> STATUS_INVALID_IMAGE_FORMAT`: PASS

Não executável neste container Linux:

- `1.bat`/PowerShell do Windows
- `kyty_emulator.exe` real do PC do usuário
- Vulkan numa RTX 4060 do usuário

O pacote foi estruturado para que esses testes ocorram automaticamente no Windows antes do boot do jogo.
