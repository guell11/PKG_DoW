# Validação PKG_DoW 2.2

## Passou

- Python backend compila com `py_compile`.
- Testes de `PARAM.SFO` e PKG metadata passam.
- Fixture CUSA (`CUSA01234`) é reconhecida como PS4, entra no scanner de raiz, preserva o serial e mantém o mesmo ID de biblioteca mesmo após mover a pasta.
- Descoberta dinâmica de flags do shadPS4 via `--help` foi validada com executável fixture.
- JavaScript passa em `node --check`.
- Todos os IDs acessados por `$('#...')` no JavaScript existem no HTML.
- Servidor local respondeu `200` para `/` e JSON válido em `/api/status` e `/api/vulkan`.
- O PKG de teste `PS4_LAPY20014_v1.01.pkg` continua sendo identificado corretamente como PS4/LAPY20014 e permanece marcado como conteúdo que precisa ser instalado/extraído antes do boot.
- O probe Vulkan reconheceu o loader 1.4.309 do sandbox; a criação da instância retornou `VK_ERROR_INCOMPATIBLE_DRIVER` porque este container não expõe uma GPU Vulkan utilizável. O erro é reportado pela UX em vez de ser mascarado.

## Não foi possível validar neste sandbox

- `1.bat` em Windows real.
- fallback PowerShell executado, porque não há PowerShell instalado no ambiente Linux.
- compilação do core após a mudança em `vulkanWindow.cpp`. A configuração CMake tentou baixar `xbyak` e falhou porque o subprocesso não possui acesso DNS/rede. Consulte `build_2_2_configure.log`.
- FPS real, porque não existe GPU de apresentação no sandbox.

## Resultado prático

A integração CUSA/Vulkan do launcher foi exercitada de ponta a ponta onde o ambiente permite. O patch do core é pequeno e defensivo: bounds check do índice e seleção multi-GPU por classe/memória. Não há alegação de FPS medido sem uma GPU real, porque números inventados continuam sendo números inventados, mesmo com um gradiente roxo bonito na UX.
