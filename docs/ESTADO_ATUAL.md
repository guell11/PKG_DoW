# Estado atual do repositório

Data da análise: 2026-09-02  
Escopo: árvore de primeira parte sob a raiz do repositório, incluindo `src/`, `tests/`, `tools/`, `webui/`, `CrispyDoom/`, scripts, documentação e configurações. `3rdparty/` foi inventariado como dependência, sem auditoria linha a linha de fornecedores.

## Resumo executivo

Esta é uma árvore do **KytyPS5**, isto é, um emulador em estágio inicial para executáveis PS5/Prospero, acrescida de duas interfaces de lançamento: o launcher Qt nativo e a UX local HTML/CSS/JS. O caminho de execução principal é:

```text
diretório/ELF PS5 -> loader ELF x86-64 -> bibliotecas HLE + kernel
                 -> PM4/RDNA2 guest -> IR -> SPIR-V -> Vulkan 1.3 -> GPU host
```

O suporte a PS4 não existe dentro do core C++: a UX reconhece `CUSAxxxxx` e delega um diretório/`eboot.bin` já extraído a um `shadPS4.exe` externo. PKG é somente catalogado por metadados; não há desencriptação, instalação, firmware ou chaves na árvore. Portanto, declarar que um PKG bruto de PS4 “roda no Kyty” seria incorreto.

Há bastante implementação real de memória virtual, linker ELF, PM4, recodificador de shader, Vulkan, cache de recursos, rede e sincronização. A compatibilidade ainda é limitada por instruções/formatos/estados guest explicitamente recusados, imports resolvidos por stubs e por áudio de saída que ainda é simulado.

## Método e inventário

- Ao fim da varredura, havia 480 arquivos visíveis de primeira parte fora de diretórios de build e de `3rdparty/`. `src/`, `tests/`, `tools/` e `webui/` somam 422 fontes/configurações; os testes de shader concentram uma parte substancial das cerca de 176 mil linhas C/C++/shader analisadas.
- A distribuição do código é: `src/common` (50 arquivos), loader (17), kernel (16), libs/HLE (64), GPU guest (14), GPU host (88), shader/recompiler (91), apresentação (14), launcher Qt (32), testes C++ (16), ferramentas (6) e WebUI (3).
- `CrispyDoom/` contém oito ELFs x86-64, três IWADs e imagens/manifesto. São fixtures de payload, não código do emulador.
- A árvore Git está toda não rastreada no instante da análise; este relatório descreve o conteúdo presente, e não atribui mudanças a commits inexistentes.

### Dependências de terceiros

`3rdparty/` declara SDL2, Vulkan-Headers, SPIRV-Headers/Tools, VulkanMemoryAllocator, FFmpeg, LibAtrac9, fmt, nlohmann_json, imgui, Tracy, spdlog, xxHash, magic_enum, cpuinfo, RenderDoc e winpthread (`.gitmodules`; `3rdparty/CMakeLists.txt:1-140`). Na cópia atual, somente fragmentos de `cpuinfo`, `stb`, `renderdoc` e `winpthread` estão materializados; os demais diretórios de dependências não estão populados.

Além disso, Xbyak e Zydis são buscados dinamicamente por `FetchContent` (`3rdparty/CMakeLists.txt:5-29`). Na validação de 2026-09-02, todos os submódulos declarados foram materializados nas revisões do upstream e CMake baixou Xbyak/Zydis; o configure e o link Windows passaram com Clang 22, Ninja 1.13 e Vulkan SDK 1.4.357.

## Matriz por subsistema

Legenda: **implementado** = caminho funcional identificado; **parcial** = implementado com limites de compatibilidade/fallbacks; **stub** = API presente mas sem semântica real suficiente; **ausente** = não há implementação neste core.

| Subsistema | Estado | O que existe | Evidências |
|---|---|---|---|
| Loader / CPU | **Parcial** | CLI aceita diretório ou ELF, monta `/app0`, carrega/reloca ELF, registra bibliotecas HLE e executa a thread guest. O guest é x86-64, portanto não há CPU PS5 reimplementada em IR; há emulação por exceção de SHA-NI, SSE4a e MONITORX/MWAITX para hosts que não as expõem. Imports não resolvidos podem ser redirecionados para thunks que retornam zero. | `src/main.cpp:43-80,331-355`; `src/emulator.cpp:126-218`; `src/loader/x64InstructionEmulator.cpp:773`; `src/loader/runtimeLinker.cpp:974-1052,2356-2360` |
| GPU guest / PM4 / AGC | **Parcial** | Processador de command buffers, filas, DMA/wait/write, registradores PM4 e decodificação AGC estão implementados. Há comandos/selectors e registros que abortam ou são aceitos temporariamente; isso pode causar draw ausente, glitch ou parada em títulos fora do subconjunto coberto. | `src/graphics/guest_gpu/graphicsRun.cpp:300-445,1512`; `src/graphics/guest_gpu/command_processor/pm4Handlers.cpp:1073,1520-1570,1939-2127`; `src/libs/agc.cpp:3138,3995` |
| Recompilador de shader | **Parcial** | Pipeline completo de decode AMD/RDNA2, CFG, IR tipada, passes de SSA/resource tracking/bindings e backend SPIR-V. O build compila shaders auxiliares GLSL para SPIR-V. Famílias ou modificadores não mapeados retornam `UNSUPPORTED`; estados de shader também têm guardas fatais para configurações ainda não modeladas. | `CMakeLists.txt:250-306`; `src/graphics/shader/recompiler/ShaderRecompiler.cpp`; `src/graphics/shader/recompiler/frontend/decode/MemoryOps.cpp:129-411`; `src/graphics/shader/recompiler/frontend/decode/VectorAluOps.cpp:1540-1852`; `src/graphics/shader/shader.cpp:223-390` |
| GPU host / recursos / render | **Parcial** | Backend Vulkan com VMA, scheduler por timeline, page/memory tracking, buffer/texture caches, tiler compute, render targets, descriptors e pipelines. Existem cache de descriptors por submission, cache de pipeline em disco e compilação assíncrona de pipeline gráfico; compute pipeline continua síncrono. Persistem caminhos fatais para MSAA/formatos/views/cópias e estados depth/stencil não cobertos. | `src/graphics/host_gpu/vma.cpp:66-93,204-265`; `src/graphics/host_gpu/renderer/pipeline/descriptorHeap.cpp:49-150`; `src/graphics/host_gpu/renderer/pipeline/pipelineCache.cpp:133-183,547-743,757-789`; `src/graphics/host_gpu/renderer/cache/textureCache.cpp:439-446,564-685,1069,1539`; `src/graphics/host_gpu/renderer/depthRenderTarget.cpp:37-275` |
| Apresentação / vídeo / input | **Parcial** | Janela SDL/Vulkan, swapchain, vblank/flip/eventos de VideoOut, fullscreen, Fifo/Mailbox/Immediate, gamepad e overlay IME estão presentes. Exige uma fila única graphics+compute+present e recursos Vulkan 1.3 específicos. Algumas superfícies, formatos, flip modes e eventos são recusados. | `src/main.cpp:54-64`; `src/graphics/presentation/window/vulkanWindow.cpp:68-73,120-145,162-172,231-270,463-496`; `src/graphics/presentation/videoOut.cpp:293,520-572,1459`; `src/graphics/presentation/imeOverlay.cpp:768-800` |
| Memória / kernel | **Parcial** | Espaço virtual guest, alocação flexível/direta, proteção, mapeamento, filesystem montado, descritores, pthreads, semáforos, event flags/queues e SyncOnAddress possuem implementação. Linux usa futex e demais plataformas usam registro portátil de waiters. Alguns contratos e variantes de sistema abortam por `EXIT_NOT_IMPLEMENTED`; Windows requer `VirtualAlloc2`. | `src/kernel/memory.cpp`; `src/kernel/fileSystem.cpp:38-92,206-233`; `src/kernel/eventFlag.cpp:101-377`; `src/kernel/syncOnAddress.cpp:78-130,187-290`; `src/common/platform/sysWindowsVirtual.cpp:130,246`; `src/kernel/semaphore.cpp:258` |
| Libs / syscalls HLE | **Parcial** | `InitAll` registra Audio, libc, libkernel, AGC, vídeo, pad, rede, save data, user/system service, fontes, PNG, PlayGo, IME e outras bibliotecas por NID. Há implementações substanciais de arquivos, rede/socket e controller, mas muitos serviços têm apenas inicialização/validação/valores de sucesso, e imports desconhecidos viram stubs do linker. | `src/libs/libs.cpp:73-131`; `src/libs/libKernel.cpp:3299-3308`; `src/libs/network.cpp:1175-2127,2293-2877,3108-3830`; `src/libs/controller.cpp:546-762`; `src/loader/runtimeLinker.cpp:300-341` |
| Áudio / vídeo decode | **Stub / parcial** | Há AudioOut/Audio3D/NGS2, AJM, ATRAC9/FFmpeg e AVPlayer no código. Porém AudioIn e AudioOut explicitamente simulam atraso porque entrada/saída real ainda não foram implementadas; custom submixer também é identificado como stub. AJM registra codecs não implementados. | `src/libs/audio.cpp:604,1128,1945-1946,2750`; `src/libs/ajm.cpp:513,928,967`; `src/libs/avPlayer.cpp:1045` |
| Launcher Qt | **Implementado, não validado em build local** | Interface Qt para biblioteca/configuração/input/patches/troféus. É um alvo separado que depende do core e de Qt6 Concurrent/Network/Widgets. | `src/launcher/CMakeLists.txt:2-41`; `src/launcher/src/mainDialog.cpp`; `src/launcher/forms/main_dialog.ui` |
| WebUI / launcher local | **Implementado** | `1.bat` prioriza Python 3.10+ e tem fallback PowerShell; o servidor local expõe biblioteca, importação, configuração, descoberta de cores, probe Vulkan e launch. A UX é estática HTML/CSS/JS e o listener padrão é `127.0.0.1`. | `1.bat:8-69`; `tools/pkg_dow_server.py:32-83,251-335,419-552,740-827,1086-1096`; `webui/index.html`; `webui/app.js`; `webui/styles.css` |
| PS4 / shadPS4 | **Ausente no core; parcial no launcher** | O launcher procura um `shadPS4.exe` configurado ou nos caminhos candidatos e pode passar `eboot.bin` ou CUSA. Não há fonte/binário shadPS4 dentro do repositório nem emulação PS4 em `src/`. | `tools/pkg_dow_server.py:320-335,753-791`; `src/emulator.cpp:207-217` |
| PKG | **Parcial (metadados apenas)** | Parser lê `PARAM.SFO`; para `.pkg`, procura strings nos primeiros 64 MiB e marca como “Precisa instalar/extrair”. Não há decriptação nem instalação. | `tools/pkg_dow_server.py:111-177,251-290,753-759`; `LEIA-ME_PKG_DoW.md:50-53` |
| Testes / ferramentas | **Parcial** | Há 16 executáveis de teste C++ registrados por CTest, testes Python de API/PKG e um harness de regressão. O harness valida assets do CrispyDoom sem tentar executar ELF de console no Windows host; core/CTest só roda se houver build CMake configurado. | `CMakeLists.txt:389-614`; `tools/test_pkg_dow_server.py:87-166`; `tools/test_crispydoom_assets.py:24-67`; `tools/run_regression.py:103-138`; `Teste_Regressao.bat:1-28` |

## Detalhes de compatibilidade e limites

### Loader, formatos de jogo e execução

- O CLI de `kyty_emulator` aceita somente uma pasta ou um ELF existente para `--game`, não um PKG (`src/main.cpp:187-206`).
- `Emulator::Run` monta a pasta como `/app0` e `/hostapp`, carrega `sce_sys/param.json`, inicializa subsistemas, carrega o ELF e chama o runtime linker (`src/emulator.cpp:192-217`).
- O linker suporta relocations e TLS, mas recusa combinações de dynamic tags e REL que não sejam o subconjunto esperado (`src/loader/runtimeLinker.cpp:2180-2243`). Quando a resolução é fraca/não obrigatória, cria um stub de import, registra e retorna `0` (`src/loader/runtimeLinker.cpp:300-341,991-1035`). Esse comportamento favorece boot, mas pode ocultar uma syscall essencial até produzir crash ou lógica incorreta posteriormente.

### Renderização e shaders

- O dispositivo Vulkan exige `dynamicRendering` e `synchronization2` (`src/graphics/presentation/window/vulkanWindow.cpp:68-73`), além de timeline semaphores, extensões/formats e recursos adicionais avaliados durante seleção (`src/graphics/presentation/window/vulkanWindow.cpp:231-270`).
- A seleção automática de GPU pontua tipo de dispositivo e VRAM local; `--gpu` inválido cai para seleção automática em vez de indexar fora do vetor (`src/graphics/presentation/window/vulkanWindow.cpp:162-172,463-496`).
- O cache de pipeline persiste por título/GPU/driver e o pipeline gráfico é produzido numa thread em background. O render thread só recebe um ticket pendente, permitindo sobrepor preparação de descriptors/uploads/render targets; isso melhora stutter potencial, mas não equivale a FPS medido (`src/graphics/host_gpu/renderer/pipeline/pipelineCache.cpp:332-468,681-743`).
- VMA consulta `VK_EXT_memory_budget` quando disponível e aplica uma política adaptativa. Buffer/texture cache fazem GC guiado por essa informação (`src/graphics/host_gpu/vma.cpp:204-265`; `src/graphics/host_gpu/renderer/cache/bufferCache.cpp:230,736`; `src/graphics/host_gpu/renderer/cache/textureCache.cpp:75,1885`).
- Os render bugs mais prováveis no estado presente são, por evidência do próprio código: shader opcode/modifier não suportado, estado de shader/PM4 aceito de forma temporária, formato/view/tiling ainda não coberto, readback de depth/textura parcial e draw pulado por estágio gráfico não implementado. Não são hipóteses de interface: são caminhos que registram aviso, pulam draw ou chamam `EXIT`.

### Kernel, HLE e áudio

- `EXIT_NOT_IMPLEMENTED` não significa sempre “módulo vazio”: uma parte é verificação de precondição. Porém, os casos que codificam uma limitação real precisam ser tratados como barreiras de compatibilidade, sobretudo em `shader.cpp`, decoder, PM4, texture cache, linker e áudio.
- Filesystem e sincronização possuem lógica real (mounts, fd table, tempo, futex/condvar). As 1.222 ocorrências de `EXIT`/`EXIT_IF`/`ASSERT` no código são majoritariamente validações; não devem ser contadas como 1.222 stubs sem inspeção de contexto.
- A camada HLE é ampla em superfície, mas não equivalente ao SO PS5. Funções de serviços online, troféus, DRM, módulos e APIs específicas podem registrar apenas init/estado local ou acabar em import stub.
- Áudio é um bloqueador funcional importante: embora formatos, portas e NGS2 sejam modelados, a saída não chega a um dispositivo de áudio host (`src/libs/audio.cpp:1128`).

## TODO, FIXME e limites explicitamente marcados

| Área | Marca / impacto | Evidência |
|---|---|---|
| Linker | Não verifica se a tabela PLT customizada já foi gerada pelo compilador. | `src/loader/runtimeLinker.cpp:2301` |
| Windows filesystem | Abrir diretório e `dwDesiredAccess = 0` continuam pendentes. | `src/common/platform/sysWindowsFileIO.cpp:408-409` |
| Save data | Diretório de save precisa ser definido pelo launcher. | `src/libs/libSaveData.cpp:26` |
| Unwind guest | Caminho de unwind sobre retorno host está pendente. | `src/libs/libKernel.cpp:1534` |
| Audio | Entrada e saída simulam atraso; custom submixer não é implementado. | `src/libs/audio.cpp:604,1128,1945-1946` |
| Shader ISA | Decoders rejeitam opcodes não mapeados de SOP*, VOP*, SMEM/MUBUF/MTBUF/FLAT/DS e vários SDWA/DPP/VOP3. | `src/graphics/shader/recompiler/frontend/decode/ScalarAluOps.cpp:137-293`; `MemoryOps.cpp:240-411`; `VectorAluOps.cpp:1543-1852` |
| Estado gráfico | Diversos bits de registradores guest permanecem guardados por `EXIT_NOT_IMPLEMENTED`; GE shader não suportado pode pular draw. | `src/graphics/shader/shader.cpp:223-390`; `src/graphics/host_gpu/renderer/renderDraw.cpp:462-481` |
| Textura/depth | Formatos, views, multisample e alguns downloads parciais abortam como não suportados. | `src/graphics/host_gpu/renderer/image/textureCommon.cpp:76,209-233`; `textureCache.cpp:564-685,1069,1539`; `imageView.cpp:298` |
| PM4/AGC | Selectors/regs/eventos específicos são recusados ou aceitos temporariamente. | `src/graphics/guest_gpu/graphicsRun.cpp:327-428,1512`; `pm4Handlers.cpp:1073,1520-1570`; `src/libs/agc.cpp:3138,3995` |

## Build, execução e riscos técnicos

1. **Build reproduzível localmente.** O core Windows foi configurado e compilado em `_Build/windows` com `clang-cl`, Ninja e Vulkan SDK; `kyty_emulator.exe` foi gerado. `Preparar_Core.bat` instala as ferramentas oficiais via WinGet, inicializa submódulos quando necessário e repete configure/build/CTest. O alvo Qt permanece desligado nesse bootstrap, pois a UX HTML local é o launcher padrão.
2. **Core disponível; launcher Qt e shadPS4 não.** A UX detecta `_Build/windows/kyty_emulator.exe` automaticamente. Não há `launcher.exe` Qt, nem `shadPS4.exe` no checkout; PS4 continua exigindo o binário externo configurado pelo usuário.
3. **Interface pode sugerir disponibilidade sem core.** `1.bat` abre a interface estática como último fallback, mas botões de launch não funcionam sem Python/PowerShell e sem executáveis configurados (`1.bat:62-69`). A API também precisa de um executável encontrado para lançar um jogo (`tools/pkg_dow_server.py:759,795`).
4. **PS4 depende de software externo.** A UX pode encaminhar PS4, mas não instala PKG nem fornece o shadPS4. Um título PS4 extraído só deve ser testado com um shadPS4 fornecido/configurado pelo usuário.
5. **Sem benchmark responsável.** Há core compilado e Vulkan 1.4 detectado em uma RTX 4060, mas não há jogo PS5 extraído que alcance o frame loop nem `shadPS4.exe` configurado. Qualquer número de FPS continuaria sendo especulação.
6. **Risco de compatibilidade de renderer.** A estratégia correta de otimização é reduzir trabalho redundante em Vulkan/IR/cache/sincronização; emitir ISA proprietária de GPU host não é parte desta árvore. O próprio launcher exibe o caminho `Prospero/RDNA2 -> IR -> SPIR-V -> Vulkan -> driver` (`tools/pkg_dow_server.py:537-552`).

## Testes e fixtures no estado atual

### CrispyDoom

`CrispyDoom/build.sh` baixa e compila Crispy Doom contra o PS5 Payload SDK, gerando os oito payloads (`CrispyDoom/build.sh:18-76`). O manifesto seleciona diretamente os ELFs e IWADs (`CrispyDoom/homebrew.js:17-127`).

O smoke test atual valida oito ELFs 64-bit x86-64, IWADs e todos os caminhos de menu, mas deliberadamente não tenta executar payload de console no Windows host (`tools/test_crispydoom_assets.py:24-67`). Esse é um teste de integridade da fixture, não de emulação, vídeo ou FPS. Uma tentativa controlada de `crispy-doom.elf` confirmou que ele é payload PS5 SDK, não executável de jogo aceito pelo loader Kyty: encerra com `elf is not valid`, código 321. O launcher agora bloqueia esse bundle antes de abrir janela preta e preserva o motivo no log.

### Resultados mais recentes disponíveis

O smoke do launcher, a sintaxe Python/JS e o manifesto CrispyDoom passam. O core foi compilado e o CLI respondeu a `--help`. CTest final aprovou **36 de 36** cenários. A especialização sRGB foi corrigida com snapshot completo; a sincronização de imagem agora só elimina barreira em escopo de acesso idêntico, preservando transição de acesso; a fixture depth/stencil foi corrigida para a configuração HTile que o cenário exige. A UX respondeu HTTP 200, detectou core e Vulkan e agora pode ser hospedada por PyQt6 WebEngine sem abrir browser externo. Logs web aparecem no terminal e stdout/stderr da engine são duplicados no terminal e em `logs/launch-*.log`.

## Prioridade recomendada para quem corrigirá o core

1. Restaurar todas as dependências/submódulos e gerar um build Windows Vulkan reproduzível antes de aceitar qualquer ganho de FPS.
2. Rodar CTest completo e ativar validação Vulkan/Shader nos payloads CrispyDoom antes de alterar sincronização/barriers.
3. Usar dumps de PM4/shader dos títulos que falham para implementar a instrução, formato ou estado preciso; não converter `EXIT_NOT_IMPLEMENTED` em “ignorar e continuar” sem uma semântica documentada.
4. Priorizar AudioOut host, opcodes de shader encontrados nos dumps, render-target/depth formats e paths de texture cache que atualmente abortam ou pulam draw.
5. Medir frame time CPU/GPU, pipeline misses, uploads/readbacks, invalidations e page faults por título antes de qualquer nova política de performance.

## Arquivos modificados por esta tarefa

- `tests/ResourceTrackingTests.cpp` — fixture sRGB atualizado para produzir um `ResourceSnapshot` completo compatível com a API atual.
- `src/graphics/host_gpu/renderer/image/image.cpp` — elisão de barreira restringida; mudança de access mask no mesmo layout mantém dependência Vulkan necessária.
- `tests/ShaderRecompilerComputeTests.cpp` — fixture HTile stencil corrigida.
- `tools/pkg_dow_server.py` — tee de logs engine, flags de diagnóstico Vulkan/Shader, afinidade CPU host e bloqueio de payload homebrew incompatível.
- `tools/pkg_dow_desktop.py` — shell PyQt6 WebEngine local.
- `1.bat` — abre shell PyQt6; instala dependência quando ausente; não abre browser externo.
- `Preparar_Core.bat` — bootstrap Windows reproduzível de ferramentas, submódulos, build e CTest.
- `docs/ESTADO_ATUAL.md` — relatório atualizado com evidências de build e teste.
