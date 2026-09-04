# PKG_DoW HyperCore 2.4 Native

## Mudanças desta rodada

- UX principal migrada do servidor/browser para `tools/pkg_dow_qt.py`, uma aplicação PyQt6 nativa.
- `1.bat` mantém o terminal aberto, usa stdout sem buffer e espelha a saída completa do core tanto no terminal quanto na página **Logs** e em arquivos de `logs/`.
- PyQt6 é instalado automaticamente em `runtime/python_packages` na primeira execução quando necessário.
- Engines deixaram de ser configuráveis por caminho na UX nova. O produto procura apenas locais administrados pelo projeto (`engines/ps5` e `engines/ps4`) e outputs locais de build.
- `bootstrap_engines.ps1` copia automaticamente builds locais detectadas para os diretórios administrados.
- Pré-flight de boot verifica caminho do jogo, `eboot.bin`, Vulkan e presença do core correto.
- Crash precoce (<20 s) gera diagnóstico e, para assinaturas recuperáveis, um único retry conservador (Fifo, shader opt None, GPU automática, PlayGo fallback e readback linear).
- Guest printf e shader logs são forçados para `Console` no Kyty para maximizar a observabilidade do crash.

## Controles

A UX agora contém um editor nativo de bindings de teclado/mouse para o pad guest. Os bindings são enviados automaticamente ao Kyty usando `--keymap`.

O core já usa SDL GameController. Xbox/XInput, DualShock/DualSense e dispositivos genéricos reconhecidos pelo SDL são conectados diretamente, sem precisar de um driver virtual adicional. O painel também enumera XInput e dispositivos PnP visíveis no Windows.

Foram feitas duas correções de estabilidade no core de input:

1. Falha ao abrir um HID como `SDL_GameController` não chama mais `EXIT_NOT_IMPLEMENTED` e não derruba o jogo.
2. Evento de remoção duplicado/desconhecido não aborta mais o emulador.

Também foi criado `--controller-deadzone 0-40`. A deadzone é aplicada nos sticks físicos com reescala do restante do range, evitando drift e spam de estados de pad sem reduzir o alcance máximo.

## Regression test DOOM/crupsti

`tools/regression_doom.py` procura automaticamente uma pasta contendo `doom`, `crupsti` ou `crispy` com `eboot.bin`, executa uma janela de smoke test e salva log/JSON em `logs/`.

No workspace desta rodada essa pasta não estava presente, então o resultado verificável foi `SKIP` em vez de inventar um benchmark.
