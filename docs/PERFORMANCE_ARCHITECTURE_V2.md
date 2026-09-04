# PKG_DoW / KytyPS5 — Arquitetura de Performance v2

Data: 2026-09-02

## Objetivo

Esta revisão tenta ganhar FPS e, principalmente, reduzir *stutter* e instabilidade sem trocar correção por placebo. Em emulação, remover uma sincronização “porque ficou mais rápido” costuma funcionar magnificamente até a primeira textura que depende dela. O objetivo desta fase foi atacar custos repetitivos comprováveis e melhorar o launcher sem inventar compatibilidade que o core ainda não possui.

## Referências pesquisadas

- KytyPS5 upstream / developer information: https://github.com/KytyPS5/KytyPS5
- AMD RDNA Performance Guide: https://gpuopen.com/learn/rdna-performance-guide/
- Khronos Vulkan Pipeline Cache guide: https://docs.vulkan.org/guide/latest/pipeline_cache.html
- Khronos Pipeline Management sample: https://docs.vulkan.org/samples/latest/samples/performance/hpp_pipeline_cache/README.html
- Khronos Descriptor Management sample: https://docs.vulkan.org/samples/latest/samples/performance/descriptor_management/README.html
- Khronos Descriptor Indexing guide: https://docs.vulkan.org/guide/latest/extensions/VK_EXT_descriptor_indexing.html
- Khronos Synchronization 2 / frame pacing: https://docs.vulkan.org/tutorial/latest/Building_a_Simple_Engine/Advanced_Topics/Synchronization_2_Frame_Pacing.html
- shadPS4 releases, como referência de arquitetura de emulador Vulkan em produção: https://github.com/shadps4-emu/shadPS4/releases
- Dolphin Hybrid Ubershaders, referência clássica para reduzir stutter de compilação: https://dolphin-emu.org/blog/2017/07/30/ubershaders/

## Conclusões da pesquisa

### 1. Cache persistente é obrigatório para uma boa segunda execução

Khronos documenta que criação de pipelines pode ser cara e que `VkPipelineCache` deve ser reaproveitado entre execuções. A árvore já possuía um cache de driver bem estruturado, porém o desativava em builds não-Release, sem revisão Git ou marcadas como dirty. Um ZIP sem `.git` cai justamente no caso `unknown`, então o usuário que compila a árvore entregue perde o benefício.

**Alteração aplicada:** o cache agora permanece habilitado em builds de arquivo/ZIP. A assinatura `KytyPC2` usa UUID de pipeline do dispositivo, vendor ID, device ID, driver version e revisão Git quando disponível; quando não existe metadata Git usa a etiqueta estável `archive`. O payload continua protegido por XXH3 e é invalidado quando a assinatura/driver não bate.

**Efeito esperado:** menos recompilação interna do driver e menos picos de frame time em execuções posteriores. Não altera a semântica do guest.

### 2. O lookup de especializações de shader não deve escanear combinações irrelevantes

`ProgramCache` agrupava cada shader guest por estágio/hash, mas procurava as especializações estáticas em um vetor linear. Jogos que produzem muitas combinações de recursos podem transformar uma busca quente em O(N) repetidamente.

**Alteração aplicada:** cada source shader agora possui buckets por hash XXH3 da chave estática. Dentro do bucket ainda há comparação exata da chave e chamada a `MaterializeProgram`, então colisões não mudam correção. O caminho comum passa a visitar apenas candidatos com a mesma chave hash.

Também foram reservados buckets iniciais para os mapas de shader/pipeline para reduzir `rehash` e alocações durante gameplay.

### 3. Blend constants não pertencem à chave de pipeline

Vulkan expõe blend constants como estado dinâmico core. A implementação anterior colocava os quatro floats de blend constant em `PipelineStaticParameters`, portanto qualquer animação desses valores gerava uma variante de pipeline diferente.

**Alteração aplicada:**

- `vk::DynamicState::eBlendConstants` foi adicionado ao pipeline;
- `vkCmdSetBlendConstants` é emitido no estado dinâmico do draw;
- os quatro valores são normalizados antes de construir a chave de pipeline.

**Efeito esperado:** menos pipelines redundantes, menor pressão no cache e menos stutter em jogos/efeitos que mudam blend constants frequentemente.


### 4. Canonicalização de blend state ignorado pelo Vulkan

Quando `blendEnable` está efetivamente desligado, os fatores/operações de blend não mudam o resultado do pipeline Vulkan. A chave anterior ainda carregava esses registradores guest e o criador de pipeline ainda tentava traduzi-los, mesmo sendo irrelevantes. Isso gerava variantes redundantes e podia transformar um valor guest não suportado em abort durante um estado onde blending nem estava ativo.

**Alteração aplicada:** estados de blend inativos agora são canonicalizados antes do lookup/criação do pipeline. Quando separate-alpha está desligado, os registradores alpha que apenas espelham o caminho color também são removidos da chave.

**Efeito esperado:** menos PSOs equivalentes e menos falhas provocadas por estado de blend irrelevante. A mesma linha de otimização aparece nas mudanças recentes do shadPS4, que passou a ignorar parâmetros de blend quando desabilitados.

### 5. Hash da chave de pipeline foi barateado

A estrutura estática, explicitamente `#pragma pack(1)` e validada como trivially copyable, era misturada byte a byte no hash. Agora ela usa XXH3 em bloco, mantendo comparação exata em caso de colisão.

**Efeito esperado:** redução pequena, porém recorrente, de custo CPU no lookup do pipeline.

### 6. Diagnóstico e telemetria local são melhores que “otimização no escuro”

O `PipelineCache::Save()` agora registra estatísticas de shaders compilados, hits/misses e quantidade de pipelines gráficos/compute. Esses números aparecem no log do core e ajudam a identificar títulos que explodem o número de variantes.

A UX também passa a mostrar:

- presença do runtime Vulkan;
- tamanho e número de arquivos do pipeline cache;
- processos de engine em execução;
- exit code do último processo;
- tempo acumulado de execução;
- diagnóstico de RAM, disco, Python e engines.

## Presets de execução da UX

### Estável

- FIFO
- shader optimizer `None`
- 1280×720
- 60 Hz virtual
- proteção SysV red-zone habilitada no Windows

Use para primeiro boot e investigação de compatibilidade.

### Balanceado (padrão)

- FIFO
- shader optimizer `Performance`
- 1280×720
- 60 Hz virtual
- proteção SysV red-zone habilitada no Windows

É o perfil recomendado após o primeiro boot. FIFO evita transformar “FPS” em tearing e frame pacing ruim só para o contador parecer mais corajoso.

### Turbo

- Mailbox
- shader optimizer `Performance`
- 960×540
- 60 Hz virtual
- proteção SysV red-zone habilitada no Windows

Reduz carga de GPU e tenta menor latência quando Mailbox é suportado. Não cria potência computacional do nada, infelizmente a física continua instalada.

## Launcher/UX de um clique

`1.bat` agora é o ponto de entrada principal. Ele tenta, nesta ordem:

1. um runtime Python portátil em `runtime/python/python.exe`, caso o usuário decida colocá-lo no pacote;
2. Python 3.10+ já instalado (`py -3` ou `python`);
3. **Windows PowerShell**, usando `tools/pkg_dow_server.ps1` como servidor local sem dependências externas.

Assim, em um Windows 10/11 normal a UX continua utilizável mesmo sem Python instalado. O backend Python permanece preferido por ter parser de `PARAM.SFO`/PKG mais completo e monitor de processos por thread. O fallback PowerShell mantém biblioteca, configurações, importação por diálogo nativo, launch dos cores, diagnósticos, cache e abertura de pastas.

A interface continua servida somente em `127.0.0.1`; não há listener de rede pública.

Também foi adicionada uma **camada de perfil por jogo**. Cada item da biblioteca pode herdar a configuração global ou fixar `Estável`, `Balanceado` ou `Turbo`, sem obrigar o usuário a trocar a configuração de todos os títulos. Isso é especialmente útil em emulação, onde um hack/limite que estabiliza um jogo pode só desperdiçar desempenho em outro.

## Camadas recomendadas para uma fase futura

Estas ideias são tecnicamente fortes, mas não foram ligadas nesta entrega porque exigem build completo, validação Vulkan e testes de jogos reais antes de serem seguras:

1. **Compilação assíncrona de pipelines** usando `VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT`/cache-control e uma fila de workers. O draw poderia usar fallback/skip temporário somente em um modo opcional. Dolphin demonstra a ideia com shaders híbridos; aplicar diretamente a RDNA2 exige cuidado para não perder efeitos render-to-texture.
2. **Pipeline libraries / shader objects** onde drivers suportarem, para desacoplar partes de pipeline e reduzir custo de variantes.
3. **Descriptor indexing / bindless** para diminuir updates e binds de descriptor sets. A migração precisa de invariantes de lifetime e sincronização rigorosos.
4. **Upload assíncrono + timeline semaphore** para staging de buffers/texturas sem travar a thread de render. Deve ser guiado por dependências reais do guest, não por múltiplas queues por entusiasmo.
5. **Synchronization2/barrier audit** para reduzir barriers redundantes e layouts `GENERAL`, seguindo o guia AMD. Essa é uma das maiores oportunidades de FPS, mas também uma das maneiras mais eficientes de criar flicker se feita sem captures/validation.
6. **Cache de SPIR-V/materialização em disco**. O core já precisa preservar metadata de recursos e outputs do IR; cachear apenas o binário SPIR-V seria incompleto. Uma implementação correta deve versionar IR/resource snapshots e invalidar por hash do guest shader + static specialization key + versão do recompiler.
7. **Perf counters por título**: frame time CPU/GPU, pipeline creates/frame, bytes de upload/readback, invalidations de texture/buffer cache e page faults. Otimização de emulador sem esses números é adivinhação com C++.
8. **Sincronização contínua com upstream**: o KytyPS5 está recebendo releases e correções de ISA/shader ativamente em 2026. Para compatibilidade, o fork deve manter as otimizações locais em commits pequenos e rebaseáveis, em vez de congelar uma cópia e tentar reimplementar sozinho cada instrução RDNA2 que o upstream já corrige.

## Limites desta entrega

- O sandbox não possui os submódulos/dependências completos necessários para gerar um novo binário Windows do KytyPS5. Portanto as mudanças C++ foram revisadas estaticamente, mas não executadas em GPU real neste ambiente.
- A árvore KytyPS5 entregue continua sendo um core PS5/Prospero. Ela não se transforma em um core PS4 completo por causa de otimizações de renderer.
- O PKG `LAPY20014` continua útil como fixture de metadata/importação, mas não pode validar o boot do core PS5. Para PS4, a UX permite apontar para shadPS4 e iniciar um jogo já instalado/extraído.
- O segundo PKG mencionado anteriormente não está presente nos arquivos desta conversa.

## Arquivos C++ alterados nesta fase

- `src/graphics/host_gpu/renderer/pipeline/pipelineCache.cpp`
- `src/graphics/host_gpu/renderer/pipeline/pipelineCache.h`
- `src/graphics/host_gpu/renderer/pipeline/shaders.cpp`
- `src/graphics/host_gpu/renderer/renderDraw.cpp`

Além deles, permanecem as correções sRGB da fase anterior.
