# Validação 2.4

- `tools/pkg_dow_qt.py`: `python -m py_compile` PASS.
- `tools/pkg_dow_server.py`: `python -m py_compile` PASS.
- `tools/regression_doom.py`: `python -m py_compile` PASS.
- `tools/test_pkg_dow_server.py`: PASS.
- Vulkan probe do backend executado: loader 1.4.309 encontrado no container; `vkCreateInstance` retorna -9 porque o container não expõe uma GPU Vulkan utilizável.
- Auditoria do repositório: 551 arquivos regulares lidos, 18,721,282 bytes; 1,029 ocorrências de `EXIT_NOT_IMPLEMENTED`, 46 marcadores `not_implemented`, 49 TODO/FIXME e 1,222 assert/exit (a contagem inclui documentação/patches gerados, portanto serve como inventário, não como métrica de código puro).
- Smoke test DOOM/crupsti: SKIP, pasta de teste não presente no workspace.
- O ambiente é Linux e não possui PyQt6 nem uma GPU/Windows runtime, portanto a janela PyQt6 e os `.exe` Windows não puderam ser executados aqui. O `1.bat` inclui bootstrap automático de PyQt6 para Windows.
- Não existe `kyty_emulator.exe`/`shadPS4.exe` compilado dentro da base local entregue nesta sessão; os slots `engines/ps5` e `engines/ps4` estão prontos para builds integradas e o bootstrap copia builds locais quando presentes.

Isso valida estrutura, parsers, launcher e alterações de código-fonte, mas não constitui garantia de compatibilidade 100% de jogos ou benchmark de FPS.
