# PKG_DoW / KytyPS5 — Relatório do Orquestrador

Data da análise: 2026-09-01/02  
Base analisada: arquivo `KytyPS5-main.zip` fornecido na conversa.  
Escopo: leitura estática completa da árvore recebida, pesquisa da arquitetura, comparação com a implementação, correção focal de shader, criação da UX local e validação possível dos PKGs fornecidos.

> Este relatório separa fatos verificados de expectativas. Emulação de console é uma coleção de casos extremos usando uma GPU como máquina de estados, porque aparentemente a computação ainda não havia sofrido o suficiente.

## Resumo executivo

- A auditoria automatizada leu **527 arquivos regulares**, totalizando **18,167,093 bytes**, com SHA-256 individual por arquivo em `docs/repo_audit.json`. O diretório transitório `_Build/ci` e o próprio JSON gerado são excluídos para não auditar lixo de build como se fosse código-fonte.
- O core recebido é **KytyPS5**, hoje focado em **PS5/Prospero**, não um core PS4 completo. O carregador contém `EXIT_NOT_IMPLEMENTED(!is_shared && !is_next_gen)` para executáveis principais que não sejam "next gen", o que bloqueia um ELF de jogo PS4 normal nessa árvore.
- O CLI do core aceita `--game <dir|elf>`, não um `.pkg` bruto. Não há instalador geral de PKG de jogo no código recebido.
- O maior débito de implementação está em PM4/AGC, bibliotecas HLE, áudio, shader/renderer e sincronização. Foram encontrados **1005** usos de `EXIT_NOT_IMPLEMENTED` sob `src/`. Isso não significa 1005 APIs únicas, pois o macro também aparece em guardas de combinações/estados ainda não cobertos, mas é um indicador objetivo de superfície incompleta.
- Foi aplicado um patch de correção **sRGB estreito** para `k8Srgb` e `k8_8Srgb`: quando Vulkan só oferece fallback UNORM, o shader SPIR-V aplica sRGB→linear e a especialização é invalidada quando o descriptor muda. O diff isolado está em `docs/shader_narrow_srgb_fix.patch`.
- Foi criada uma UX HTML/CSS/JS local em `webui/`, servida por `tools/pkg_dow_server.py`, e um único ponto de entrada para Windows: `PKG_DoW.bat`. A biblioteca é local e vazia por padrão. O usuário importa seus próprios jogos.
- O primeiro PKG fornecido foi reconhecido estaticamente como **PS4 Player 3D / LAPY20014**. Ele não foi "bootado" porque a árvore KytyPS5 recebida não instala PKG PS4 e não existe binário Windows compilado dentro do ZIP recebido. O segundo arquivo LAPY20009 não foi fornecido no sandbox, então é impossível validar seus bytes ou execução sem inventar resultado.

---

# Agente 1 — Pesquisador de Arquitetura de Hardware

## Plataforma PS4

Referências primárias consultadas:

- PlayStation PS4 Technical Specifications: https://www.playstation.com/pt-br/ps4/tech-specs/
- PlayStation PS4 FAQ: https://blog.playstation.com/2013/10/30/ps4-the-ultimate-faq-north-america/
- AMD Sea Islands ISA, documento 70653: https://docs.amd.com/v/u/en-US/sea-islands-instruction-set-architecture_0

Mapa relevante:

- CPU: x86-64 AMD Jaguar, 8 núcleos. Isso reduz a necessidade de um tradutor de ISA de CPU completo em host x86-64, mas não elimina diferenças de ABI, TLS, syscalls, mapeamento de memória, instruções opcionais e comportamento do SO.
- GPU: Radeon custom, 18 Compute Units, 1,84 TFLOPS no PS4 base. A documentação Sea Islands/GCN cobre famílias de encoding e semântica de instruções como SOP*, VOP*, SMRD/VMEM/DS/EXP, wavefront, registros SGPR/VGPR, EXEC/VCC/SCC e sincronização.
- Memória: 8 GB GDDR5 unificada; a FAQ oficial documenta 176 GB/s. Para um emulador, coerência entre memória guest, páginas host, buffers Vulkan, image layouts e caches é mais importante que imitar a largura de banda física literalmente.
- Pipeline: command processor/PM4 + registradores de contexto/configuração + shaders GCN. A fidelidade depende de interpretar corretamente pacote PM4, estado de registradores, tiling/swizzle, formatos, barriers, depth/stencil, blend e semântica de shader.

### Comparação com a árvore recebida

A árvore recebida não é um backend PS4 GCN completo. Os nomes e tipos de `src/graphics/guest_gpu` são majoritariamente Prospero/PS5. O bloqueio explícito de executável PS4 no runtime linker confirma que tentar "forçar" LAPY20014 pelo core atual não seria uma correção pequena, seria abrir outra frente de compatibilidade inteira.

## Plataforma PS5

Referências primárias consultadas:

- Especificações oficiais PS5: https://blog.playstation.com/2020/03/18/unveiling-new-details-of-playstation-5-hardware-technical-specs/
- AMD RDNA 2 ISA, documento 70648: https://docs.amd.com/v/u/en-US/rdna2-shader-instruction-set-architecture

Mapa relevante:

- CPU: AMD Zen 2 x86-64, 8 cores / 16 threads, até 3,5 GHz.
- GPU: RDNA 2 custom, até 2,23 GHz / 10,3 TFLOPS, com aceleração de ray tracing.
- Memória: 16 GB GDDR6 e 448 GB/s.
- Em shader RDNA2, a distinção entre execução escalar/vetorial, máscaras de lanes, EXEC/VCC, formatos de instrução, LDS, atomics, image/buffer ops e barriers precisa sobreviver à tradução para a IR e depois SPIR-V.

### Mapeamento código ↔ arquitetura

- `src/graphics/guest_gpu/command_processor`: interpretação de PM4 e atualização do estado guest.
- `src/graphics/guest_gpu/gpu_defs.h`, `gpu_format.*`, `hardwareContext.h`: formatos, primitivos, blend/depth/stencil, descriptors e estado gráfico.
- `src/graphics/shader/recompiler/frontend/decode`: decodificação de opcodes RDNA2.
- `frontend/translate`: semântica das instruções para a IR.
- `frontend/cfg` + `ir/passes`: fluxo de controle, SSA, propagação, tracking/materialização de recursos e barriers.
- `backend/spirv`: emissão SPIR-V para Vulkan.
- `src/graphics/host_gpu`: recursos Vulkan, caches, imagens, buffers, sincronização e pipelines.
- `src/graphics/presentation`: swapchain/janela/video out.

### Divergências e riscos encontrados

1. **Cobertura incompleta de comandos GPU**: `graphics/guest_gpu` possui 216 guardas `EXIT_NOT_IMPLEMENTED`, concentradas em `pm4Handlers.cpp`. Um pacote/reg de estado não reconhecido pode interromper o frame ou deixar estado stale.
2. **Cobertura incompleta de instruções shader**: o decodificador possui caminhos `UNSUPPORTED`/não implementados e o conjunto shader tem 87 `EXIT_NOT_IMPLEMENTED`. Glitches podem surgir tanto na decodificação quanto na tradução/IR/SPIR-V.
3. **Sincronização host/guest**: renderer/cache/scheduler possui muitos guardas e caminhos especiais. Erros de barrier/layout/cache podem produzir flicker, textura velha, depth incorreto e corrupção intermitente.
4. **Formatos e espaço de cor**: foi encontrado um caso concreto onde `R8_SRGB`/`R8G8_SRGB` não têm suporte garantido em Vulkan. O fallback UNORM sem decode deixava amostras no espaço errado. Esse foi corrigido neste pacote.
5. **Tiling/metadata de imagem**: `tiler.cpp`, `textureCache.cpp`, depth/color targets e PM4 ainda têm casos não implementados. Isso continua sendo uma fonte provável de artefatos por título.

---

# Agente 2 — Documentador do Estado Atual

## Método

`tools/repo_audit.py` percorre recursivamente todos os arquivos regulares do projeto, lê os bytes, calcula SHA-256, classifica texto/binário, extensão e área, e conta marcadores de implementação. Resultado reproduzível: `python tools/repo_audit.py`.

Auditoria final desta fase: **527 arquivos**, **18,167,093 bytes**. Os totais de marcadores de débito citados neste relatório usam `source_marker_totals`, para que textos de documentação que mencionam o macro não inflem a métrica.

## Estado por subsistema

| Área | Estado observado | Evidência objetiva |
|---|---|---|
| Common / plataforma | Funcional com caminhos específicos de SO | 50 arquivos; 9 EXIT_NOT_IMPLEMENTED; 1 marcadores textuais de não implementação |
| Loader / runtime linker | ELF, símbolos, patches, JIT stubs e emulação de algumas instruções x64; incompleto | 17 arquivos; 48 EXIT_NOT_IMPLEMENTED; 0 marcadores textuais de não implementação |
| Kernel / memória / FS / pthread | Implementação substancial, mas muitos comportamentos ainda guardados | 16 arquivos; 72 EXIT_NOT_IMPLEMENTED; 0 marcadores textuais de não implementação |
| GPU guest / PM4 | Parcial; command processor grande, muitos opcodes/estados faltantes | 14 arquivos; 216 EXIT_NOT_IMPLEMENTED; 0 marcadores textuais de não implementação |
| Shader recompiler | Pipeline real decode→IR→passes→SPIR-V, porém cobertura ainda parcial | 91 arquivos; 87 EXIT_NOT_IMPLEMENTED; 29 marcadores textuais de não implementação |
| Vulkan host renderer | Recursos/caches/pipeline/sync/tiling implementados, com casos incompletos | 88 arquivos; 169 EXIT_NOT_IMPLEMENTED; 0 marcadores textuais de não implementação |
| Presentation / VideoOut | Janela/swapchain/video out presentes, casos ainda guardados | 14 arquivos; 36 EXIT_NOT_IMPLEMENTED; 0 marcadores textuais de não implementação |
| Áudio | SDL + ATRAC9 e estruturas de portas existem; `audio.cpp` sozinho tem 125 EXIT_NOT_IMPLEMENTED | 64 arquivos; 365 EXIT_NOT_IMPLEMENTED; 1 marcadores textuais de não implementação |
| Vídeo | `VideoDec2` usa FFmpeg para AVC/HEVC/VP9; integração HLE depende de submódulos/deps | incluído em `libs` |
| Input | SDL pad/controller com rumble e mapeamentos presentes | incluído em `libs` |
| Rede | sockets host e wrappers HLE presentes, 36 EXIT_NOT_IMPLEMENTED em `network.cpp` | incluído em `libs` |
| Save data / user / dialogs | APIs HLE presentes, parcialmente implementadas | incluído em `libs` |
| Launcher Qt original | Implementação de GUI existente sem marcadores de stub no conjunto auditado | 32 arquivos; 0 EXIT_NOT_IMPLEMENTED; 0 marcadores textuais de não implementação |
| Testes | 16 executáveis/arquivos de regressão, incluindo shader/resource/memory/kernel/audio | 16 arquivos; 0 EXIT_NOT_IMPLEMENTED; 7 marcadores textuais de não implementação |

## Arquivos com maior concentração de `EXIT_NOT_IMPLEMENTED`

- `src/graphics/guest_gpu/command_processor/pm4Handlers.cpp`: 186
- `src/libs/audio.cpp`: 125
- `src/libs/agc.cpp`: 123
- `src/graphics/host_gpu/renderer/debug.cpp`: 99
- `src/graphics/shader/shader.cpp`: 86
- `src/loader/runtimeLinker.cpp`: 43
- `src/kernel/pthread.cpp`: 43
- `src/libs/network.cpp`: 36
- `src/libs/ajm.cpp`: 24
- `src/graphics/presentation/window/vulkanWindow.cpp`: 22
- `src/kernel/fileSystem.cpp`: 20
- `src/graphics/guest_gpu/graphicsRun.cpp`: 20
- `src/libs/libAudio2.cpp`: 17
- `src/libs/libKernel.cpp`: 15
- `src/graphics/host_gpu/renderer/image/tiler.cpp`: 15
- `src/graphics/host_gpu/renderer/pipeline/shaders.cpp`: 14
- `src/libs/libSaveData.cpp`: 10
- `src/graphics/host_gpu/renderer/cache/textureCache.cpp`: 9
- `src/graphics/presentation/videoOut.cpp`: 8
- `src/graphics/host_gpu/renderer/renderDraw.cpp`: 8

### Leitura correta desses números

O macro não equivale automaticamente a "função inteira vazia". Em vários pontos ele protege apenas combinação de flags, formato, opcode ou estado ainda não testado. Mesmo assim, quando um jogo chega a uma dessas combinações, o processo tende a falhar em vez de degradar graciosamente. Por isso, o objetivo de estabilidade deve ser guiado por logs de jogos reais e testes regressivos, não por apagar macros no atacado. Apagar assert para "rodar" é a variante em C++ de tirar a luz do painel do carro para resolver o motor.

## Dependências faltando no ZIP recebido

`.gitmodules` referencia SDL2, magic_enum, xxHash, Vulkan-Headers, SPIRV-Tools, SPIRV-Headers, VulkanMemoryAllocator, ffmpeg-core, fmt, tracy, spdlog, LibAtrac9, nlohmann_json e imgui. Muitas dessas pastas não vieram populadas no ZIP. Uma configuração CMake offline falhou primeiro em `xbyak`, registrado em `docs/build_probe.log`. Logo, não existe base honesta para alegar que o C++ modificado foi recompilado dentro deste sandbox.

---

# Agente 3 — Especialista em Otimização e Shaders

## Pipeline analisado

O caminho crítico observado é:

`PM4 / registradores guest → hardware context → shader decode RDNA2 → CFG → IR → SSA/otimizações/resource tracking → SPIR-V → pipeline Vulkan → descriptor/buffer/texture caches → command scheduler/presentation`.

Pontos de custo e risco:

- recompilar shaders quando a chave de especialização muda;
- crescimento do pipeline cache e invalidação por driver/device;
- materialização de resources/indirect image tables;
- upload/readback e transições de imagens;
- tiling/deswizzle de formatos guest;
- sincronização entre memória guest e recursos GPU;
- validação Vulkan/SPIR-V em builds de debug, útil para diagnóstico mas cara para FPS;
- present mode: FIFO tende a ser o perfil seguro; Mailbox pode reduzir latência quando suportado; Immediate pode rasgar a imagem.

## Correção aplicada: narrow sRGB decode

Arquivos alterados:

1. `src/graphics/guest_gpu/gpu_format.cpp`
2. `src/graphics/guest_gpu/gpu_format.h`
3. `src/graphics/host_gpu/vulkanCommon.cpp`
4. `src/graphics/shader/recompiler/backend/spirv/spirvEmitterImage.cpp`
5. `src/graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h`
6. `src/graphics/shader/recompiler/ir/ShaderIR.h`
7. `src/graphics/shader/recompiler/ir/passes/ResourceMaterialization.cpp`
8. `tests/ResourceTrackingTests.cpp`

O patch é alinhado à correção pública KytyPS5 PR #415: https://github.com/KytyPS5/KytyPS5/pull/415

Comportamento:

- `k8Srgb` e `k8_8Srgb` continuam mapeados a UNORM no host quando o formato sRGB estreito não é confiável.
- `TextureNeedsShaderSrgbDecode()` marca esses descriptors.
- A informação entra na especialização de imagem e na validação da chave do shader.
- Sample/gather passam por uma conversão sRGB→linear piecewise usando 0,04045 / 12,92 e potência 2,4.
- Alpha de sample normal permanece linear; gather decodifica os quatro componentes retornados.
- Imagens indiretas carregam o mesmo estado, evitando misturar candidates incompatíveis silenciosamente.
- Novo teste cobre descriptor estreito, formato sRGB de quatro canais e invalidação quando o descriptor troca.

**Impacto esperado:** correção de fidelidade em caminhos que usam esses formatos, especialmente vídeo/planos estreitos. Não é vendido como ganho de FPS. O custo existe apenas nos shaders especializados que precisam do decode.

## Otimização adotada na UX

O launcher expõe perfis já aceitos pelo CLI Kyty: present mode, `--shader-optimization-type`, resolução, GPU e fullscreen. O default ficou conservador: `Fifo`, shader optimization `None`, 1280×720, GPU auto. Isso prioriza boot estável; subir resolução e forçar modos mais agressivos sem medir só converte frames em calor com convicção.

---

# Agente 4 — Testador e Validador de PKGs

## PKG 1: PS4_LAPY20014_v1.01.pkg

Arquivo recebido e inspecionado:

- tamanho: **57.475.072 bytes**
- SHA-256: **5e689636fefe29d1b003dda917b7b07631decefe869d7ec8d9770bd5848c3e2f**
- magic inicial: `7f 43 4e 54`
- Content ID detectado: `VR1234-LAPY20014_00-0000000000000000`
- Title ID: `LAPY20014`
- título detectado: `PS4 Player 3D`
- plataforma inferida: **PS4**

### Resultado

**Status: catalogação OK; boot não realizado nesta árvore.**

Motivos verificados:

1. `kyty_emulator` aceita pasta/ELF, não PKG bruto.
2. Não há instalador geral de PKG PS4 no core recebido.
3. `runtimeLinker.cpp` rejeita executável principal não-PS5/non-next-gen.
4. O ZIP enviado não contém um `kyty_emulator.exe` compilado.
5. O sandbox de análise é Linux, portanto um `.bat`/binário Windows também não poderia ser validado end-to-end aqui.

A UX trata isso corretamente: um `.pkg` PS4 pode ser catalogado, mas o botão de executar informa que ele precisa ser instalado por um core PS4 compatível. O launcher aceita configurar `shadPS4.exe`; para ELF extraído, o shadPS4 atual documenta boot direto de `/path/to/game.elf`.

## PKG 2: PS4_LAPY20009_v2.07.pkg

**Status: NÃO TESTADO — arquivo não foi enviado.**

A busca no diretório de uploads encontrou apenas `PS4_LAPY20014_v1.01.pkg`. Qualquer hash, metadata, log ou status de boot para LAPY20009 seria fabricado, o que não ajuda ninguém além da indústria de relatórios falsos.

---

# UX PKG_DoW criada

Arquivos:

- `PKG_DoW.bat`: único ponto de entrada no Windows.
- `tools/pkg_dow_server.py`: servidor localhost somente com biblioteca padrão do Python, API local, importação, biblioteca, settings, logs e launch dispatch.
- `webui/index.html`, `webui/styles.css`, `webui/app.js`: interface dark/neon inspirada na referência fornecida, sem copiar capas de jogos comerciais.
- `userdata/library.json`: vazio por padrão.
- `userdata/settings.json`: preferências locais.

Recursos:

- importar PKG/ELF ou pasta de jogo;
- ler `PARAM.SFO` básico quando disponível;
- reconhecer metadados não criptografados de PKG para catalogação;
- favoritos, recentes, busca e biblioteca;
- status de CPU/RAM/disco;
- localizar/configurar KytyPS5 e shadPS4;
- iniciar PS5 via `kyty_emulator --game ...`;
- iniciar PS4 extraído via shadPS4 quando configurado;
- abrir pasta, verificar caminhos, limpar apenas cache da UX;
- logs de launch separados por jogo;
- nenhuma chave, firmware ou jogo incluído.

## Validações feitas na UX

- `python -m py_compile tools/pkg_dow_server.py`: OK.
- `node --check webui/app.js`: OK.
- parser HTML da stdlib: OK.
- smoke test HTTP em localhost: `/` e `/api/status`: OK.
- probe do PKG LAPY20014 pelo mesmo parser do launcher: OK.
- `PKG_DoW.bat`: **não executado** no sandbox Linux; sua lógica foi revisada estaticamente. Em Windows ele procura Python 3 e abre o servidor/UX.

---

# O que ainda impede a promessa "todo jogo sem bug e FPS alto"

Não existe uma correção única que transforme este estado em compatibilidade universal. A própria árvore upstream declara estágio inicial e compatibilidade limitada. Os bloqueios concretos deste ZIP são: dependências git ausentes, cobertura incompleta de PM4/shaders/HLE, casos de memória/sync/tiling não implementados e ausência de suporte direto a executável principal PS4 no KytyPS5 recebido.

Para chegar a estabilidade por jogo, o ciclo correto é reproduzir → coletar log/Vulkan validation/RenderDoc → reduzir o primeiro estado/opcode divergente → corrigir → adicionar teste → medir frame time. Este pacote deixa o projeto organizado para esse ciclo e corrige um bug visual específico, mas não mascara as lacunas restantes.

# Arquivos de evidência

- `docs/repo_audit.json` — inventário SHA-256 e contagem por arquivo.
- `docs/shader_narrow_srgb_fix.patch` — diff isolado da correção de shader.
- `docs/build_probe.log` — falha de configuração causada por dependência ausente no ZIP.
- `docs/PKG_TEST_REPORT.md` — status dos PKGs e limitações de boot.
- `LEIA-ME_PKG_DoW.md` — uso rápido do pacote.
