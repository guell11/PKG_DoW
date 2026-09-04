# PKG_DoW AutoBoost 2.3 — Vulkan, RTX, VRAM e frame pacing

## Objetivo

Esta rodada altera o backend Vulkan e o launcher para reduzir CPU overhead, shader/pipeline stutter e pressão de memória sem sacrificar correção por atalhos específicos de um único driver.

A regra central é simples: o emulador traduz o **hardware guest** (Prospero/RDNA2) para uma IR e depois para **SPIR-V/Vulkan**. O driver instalado converte SPIR-V/pipeline state para a ISA nativa da GPU host. Em NVIDIA isso termina em código nativo da família Turing/Ampere/Ada/Blackwell, mas o emulador não emite SASS diretamente. Essa separação mantém compatibilidade entre drivers e modelos.

## Pesquisa host GPU

### RTX 3060

- arquitetura NVIDIA **Ampere**;
- 3584 CUDA cores;
- variantes de 12 GB ou 8 GB GDDR6;
- RT Cores de 2ª geração e Tensor Cores de 3ª geração.

Fonte oficial: https://www.nvidia.com/pt-br/geforce/graphics-cards/30-series/rtx-3060-3060ti/

### RTX 4060

- arquitetura NVIDIA **Ada Lovelace**;
- 3072 CUDA cores;
- 8 GB GDDR6;
- RT Cores de 3ª geração e Tensor Cores de 4ª geração.

Fonte oficial: https://www.nvidia.com/en-us/geforce/graphics-cards/40-series/rtx-4060-4060ti/

### RTX 5060

- arquitetura NVIDIA **Blackwell**;
- 3840 CUDA cores;
- 8 GB GDDR7 / interface de 128 bits;
- RT Cores de 4ª geração e Tensor Cores de 5ª geração.

Fonte oficial: https://www.nvidia.com/en-us/geforce/graphics-cards/50-series/rtx-5060-family/

A detecção no core é por `vendorID` + família no `deviceName`, cobrindo toda a série, não apenas esses três modelos: RTX 30 = Ampere, RTX 40 = Ada, RTX 50 = Blackwell, RTX 20/GTX 16 = Turing. O launcher mostra a mesma classificação.

## 1. Descriptor cache

O caminho sem push descriptors agora materializa uma chave exata do descriptor set: layout, binding, array element, descriptor type, buffer/view/sampler, offset, range e image layout.

- hit: reutiliza o `VkDescriptorSet` e não chama `vkUpdateDescriptorSets` novamente;
- miss: aloca/atualiza e adiciona ao cache;
- colisões de hash são verificadas por comparação exata;
- cache limitado por submission/timeline tick para evitar referência a recursos reciclados;
- limite padrão de 4096 entradas; NVIDIA usa 8192, limitado a 16384.

Isso segue a recomendação do Khronos de reutilizar descriptor sets e evitar updates redundantes em hot paths.

Fonte: https://docs.vulkan.org/samples/latest/samples/performance/descriptor_management/README.html

## 2. Pipeline e driver shader compilation em background

`BeginGraphicsPipeline()` cria a chave imutável do pipeline e agenda a criação Vulkan numa thread dedicada. Enquanto o driver compila o pipeline/shaders, a render thread continua:

1. resolvendo descriptors;
2. preparando vertex/index buffers;
3. executando uploads necessários;
4. adquirindo render targets.

O draw só chama `Wait()` imediatamente antes de ligar o pipeline. Pipelines iguais pendentes compartilham o mesmo `shared_future`, evitando compilações duplicadas.

O acesso ao mesmo `VkPipelineCache` é explicitamente serializado, pois Vulkan exige sincronização externa desse objeto. O pipeline cache persistente continua sendo salvo por título e validado por GPU/driver/schema.

Importante: o frontend guest RDNA2 -> IR/SPIR-V ainda pode ter trabalho síncrono no primeiro shader. O que foi colocado em background nesta rodada é a criação de pipeline e a compilação interna feita pelo driver, que é justamente uma fonte conhecida de runtime stutter.

Fontes:
- https://docs.vulkan.org/samples/latest/samples/performance/pipeline_cache/README.html
- https://developer.nvidia.com/blog/vulkan-dos-donts/

## 3. Uploads sem stall

O `StreamBuffer` já conhece ownership por timeline tick. Antes, vários uploads podiam esperar pelo ring. Agora esses caminhos solicitam `allow_wait=false`:

- se existe espaço livre no upload ring, usa o ring normalmente;
- se o ring ainda pertence à GPU, cria um `MemoryUsage::Upload` transitório;
- grava/copia desse spill buffer e agenda destruição somente depois do tick do scheduler.

Isso evita bloquear a render thread esperando um pedaço específico do ring. Não foi inventada uma transfer queue separada sem a infraestrutura de ownership/semaphore necessária. O scheduler atual já usa timeline semaphores, e a estratégia aplicada preserva essa ordem.

## 4. Barriers mais precisos

Duas mudanças foram aplicadas:

1. Imagens em **mesmo layout, read -> read** não emitem barrier. Os masks de stage/access são apenas mesclados.
2. Upload de buffers deixa de usar `ALL_COMMANDS -> TRANSFER -> ALL_COMMANDS`; usa `Synchronization2` e limita os estágios a graphics + compute + transfer, mantendo dependências de escrita intactas.

Nenhuma barreira de write-after-read/read-after-write/write-after-write é descartada. O objetivo é remover sincronização comprovadamente redundante, não ganhar FPS apostando que a GPU “provavelmente termina a tempo”.

Fontes:
- https://docs.vulkan.org/samples/latest/samples/performance/pipeline_barriers/README.html
- https://developer.nvidia.com/blog/vulkan-dos-donts/

## 5. VRAM dinâmica e residência

O PS5 possui memória unificada no hardware guest. Um PC com RTX não: a CPU usa RAM e a GPU dedicada usa VRAM, atravessando PCIe quando dados precisam migrar. Portanto não é correto mover toda a RAM guest para VRAM e esperar que CPU/x86 continue acessando-a como RAM normal.

O que o backend pode fazer com segurança é manter o **working set gráfico** em memória device-local pelo maior tempo possível. AutoBoost faz isso usando VMA + `VK_EXT_memory_budget`.

### Limites dedicados

| VRAM | Target | Pressure | Critical | Reserva física aproximada |
|---|---:|---:|---:|---:|
| <= 6 GB | 82% | 89% | 95% | 768 MiB |
| <= 8 GB | 86% | 92% | 96% | 640 MiB |
| <= 12 GB | 90% | 94% | 97% | 768 MiB |
| <= 16 GB | 91% | 95% | 97,5% | 1024 MiB |
| > 16 GB | 92% | 96% | 98% | 1280 MiB |

A reserva também é limitada a 25% do budget. Em UMA/integrada: 70% / 82% / 90%, com reserva de até 2 GiB.

`VK_EXT_memory_budget` é consultado em cada passagem de GC. Assim, se compositor, navegador, OBS ou outro jogo consumir VRAM e o Windows reduzir o orçamento deste processo, os thresholds caem automaticamente. Isso é intencional: NVIDIA recomenda ficar abaixo do OS memory budget para evitar demotion/paging de VRAM para system memory, que provoca stutter severo.

Fonte: https://developer.nvidia.com/blog/vulkan-dos-donts/

### Exemplos

- RTX 4060 8 GB: target nominal ~86%, cerca de 6,9 GiB antes de ajustes do budget do SO.
- RTX 5060 8 GB: mesma capacidade nominal, portanto o mesmo target de residência; Blackwell muda o perfil de arquitetura, mas 8 GB continuam sendo 8 GB.
- RTX 3060 12 GB: target nominal ~90%, cerca de 10,8 GiB.
- RTX 3060 8 GB: entra na política de 8 GB.

Isso deliberadamente não usa 100%. Encher a VRAM além do orçamento faz o Windows paginar/demover alocações para RAM, anulando a vantagem que se queria obter.

## 6. Caminho de tradução por arquitetura

AutoBoost registra e exibe:

`Prospero/RDNA2 -> Kyty IR -> SPIR-V -> Vulkan -> driver host -> ISA nativa`

O ponto de especialização confiável é antes do driver: layouts, descriptor strategy, shader specialization cache, pipeline state canonicalization, synchronization, upload policy, memory residency e extensões Vulkan. O driver NVIDIA é quem conhece instruções, scheduling, register allocation, dual issue/cache e detalhes privados da versão instalada.

Hard-code de SASS para “RTX 4060” seria menos compatível, não mais. Além de variar por Ada/Blackwell, também prenderia o emulador a detalhes não estáveis entre drivers.

## 7. UX e automação

A UX 2.3 mostra automaticamente:

- arquitetura da GPU host;
- VRAM local e target estimado;
- presença de `VK_EXT_memory_budget`, `VK_EXT_memory_priority`, pipeline cache control e descriptor buffer;
- descriptor cache target;
- pipeline background / no-stall uploads / barrier policy;
- caminho guest -> Vulkan -> driver.

`1.bat` permanece como entrada única. Python 3.10+ é preferido; Windows PowerShell continua sendo fallback local.

## Limitações verificáveis

A árvore entregue não contém o conteúdo de vários submódulos/dependências C++ (`fmt`, Vulkan-Headers, VMA, xxHash, SDL2 etc.). O configure tentou buscar `xbyak`, mas o ambiente de build não possui acesso Git de saída. Por isso esta rodada fornece o **fonte alterado**, patch, testes de launcher e logs, mas não afirma ter produzido um `kyty_emulator.exe` novo nem números de FPS que não puderam ser medidos.
