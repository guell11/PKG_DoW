# PKG_DoW 2.5 — início rápido

Este pacote contém o código KytyPS5 modificado e a UX local. **Não contém jogos, firmware, chaves nem PKGs.**

## Abrir no Windows com um arquivo

1. Extraia o ZIP.
2. Execute **`1.bat`**.
3. A UX abre no navegador em `127.0.0.1`.
4. Em **Configurações**, deixe **GPU Vulkan = Auto** para usar a política automática por hardware.
5. PS4 usa core oficial shadPS4 0.18.0 incluído em `engines/ps4/core-0.18.0`.
6. Importe somente dumps que você tenha direito de usar.

`1.bat` tenta Python 3.10+ e, se não houver, usa o fallback PowerShell incluído. `PKG_DoW.bat` chama o mesmo launcher.

## AutoBoost 2.3

A camada nova é automática. Não existe botão mágico de “RTX mode”, porque isso seria só cosplay de engenharia.

- **Descriptor cache:** reaproveita descriptor sets com chave exata de buffers/imagens e evita `vkUpdateDescriptorSets` redundante. O cache é limitado ao tick/submission atual para não reutilizar recursos reciclados.
- **Pipeline em background:** a criação do pipeline Vulkan e a compilação interna do driver começam antes da preparação de descriptors, buffers e render targets. O draw espera somente no ponto em que o pipeline realmente precisa ser ligado.
- **Pipeline cache persistente:** blobs do driver continuam persistidos por jogo e validados por GPU/driver/schema.
- **Uploads sem stall:** o upload ring não bloqueia a render thread quando ainda está ocupado pela GPU. O core usa um upload buffer transitório e o aposenta pelo tick do scheduler.
- **Barriers mais precisos:** transições leitura->leitura no mesmo layout são eliminadas; uploads de buffer usam Synchronization2 com estágios gráficos/compute/transfer em vez de `ALL_COMMANDS` dos dois lados.
- **VRAM dinâmica:** `VK_EXT_memory_budget` alimenta os limites de GC. Se Windows/driver reduzir o orçamento porque outro processo passou a consumir VRAM, o alvo do emulador cai automaticamente.
- **Arquitetura host:** RTX 20/GTX 16 = Turing, RTX 30 = Ampere, RTX 40 = Ada Lovelace, RTX 50 = Blackwell. Também há classificação genérica para AMD/Intel.

### Política de residência

Em GPU dedicada, o core tenta manter grande parte do working set em memória device-local sem encostar em 100%, porque ultrapassar o orçamento pode causar demotion/paging para RAM e destruir frame pacing:

| VRAM local | Alvo aproximado | Pressão | Crítico |
|---|---:|---:|---:|
| até 6 GB | 82% | 89% | 95% |
| 8 GB | 86% | 92% | 96% |
| 12 GB | 90% | 94% | 97% |
| 16 GB | 91% | 95% | 97,5% |
| >16 GB | 92% | 96% | 98% |

Há ainda uma reserva física mínima. Em GPU integrada/UMA a política é mais conservadora, porque “VRAM” é RAM do sistema compartilhada.

## RTX e tradução de shaders

O PS5/Prospero é RDNA2. Em uma RTX, o caminho correto é:

`shader guest RDNA2 -> IR do emulador -> SPIR-V -> Vulkan -> driver NVIDIA -> ISA nativa da GPU`

Não é correto traduzir diretamente RDNA2 para SASS de uma RTX 3060/4060/5060 dentro do emulador. SASS é ISA proprietária gerada pelo driver/compilador NVIDIA e varia com arquitetura e driver. A otimização por RTX é feita no nível que permanece estável: cache, sincronização, memória, descriptors, pipeline e SPIR-V/Vulkan, deixando a etapa final para o driver.

## Instalar PKG PS4

1. Abra **Importar jogos**.
2. Selecione **Importar PKG**.
3. Escolha arquivo no navegador interno.
4. Aguarde extração chegar a 100% dentro do PKG_DoW.
5. Abra cartão instalado e clique **Jogar**.

Instalação vai para `userdata/ps4-games`. Extração usa ferramenta CLI interna, sem abrir interface de outro emulador.

## Teclado e mouse

- `WASD`: analógico esquerdo.
- Movimento do mouse: analógico direito/câmera.
- Clique esquerdo: `R2`.
- Clique direito: `L2`.
- Clique do meio: `R3`.
- Sensibilidade: **Configurações > Controles**.
- Janela de jogo recebe título `PKG_DoW · nome do jogo`.

- IDs PS4 homebrew como `LAPY`, `NPXX` e `QUIG` têm identidade estável.
- Arquivo `PS4_NPXX51374_v1.00.pkg` declara `NPXX51362` no cabeçalho. Cabeçalho vence nome do arquivo.
- Nenhuma chave, firmware ou rotina própria de quebra de DRM fica no PKG_DoW.
- PKG incompatível ou protegido continua rejeitado pelo instalador oficial.

## Relatórios

- `docs/AUTOBOOST_2_3.md` — arquitetura, pesquisa e alterações.
- `docs/VALIDATION_2_3.md` — testes executados e limitações.
- `docs/performance_autoboost_2_3.patch` — diff desta rodada sobre a versão 2.2.

O fonte C++ precisa de uma árvore de dependências/submódulos completa para gerar um novo executável Windows. O ZIP recebido mantém várias pastas de terceiros vazias, então este ambiente não consegue honestamente afirmar que compilou e mediu FPS do binário novo.
