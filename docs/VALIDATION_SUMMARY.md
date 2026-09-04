# Validação realizada no sandbox — Boost 2.1

- `python3 -m py_compile tools/pkg_dow_server.py`: **PASS**
- `python3 tools/test_pkg_dow_server.py`: **PASS**
- `node --check webui/app.js`: **PASS**
- parse de `webui/index.html` com `html.parser`: **PASS**
- checagem de IDs usados pelo JavaScript contra IDs do HTML: **PASS**
- smoke HTTP real do launcher: `/`, `/app.js`, `/styles.css`, `/api/status`, `/api/settings`, `/api/library`, `/api/diagnostics`, `/api/logs`: **PASS**
- smoke de API com importação temporária, perfil por jogo `turbo` e `/api/verify`: **PASS**
- auditoria do repositório regenerada: **532 arquivos regulares lidos**
- `tools/pkg_dow_server.ps1`: revisão estática e incluído como fallback do `1.bat`; **NOT RUN**, pois o sandbox Linux não possui Windows PowerShell/PowerShell Core
- `1.bat`: fluxo revisado estaticamente; **NOT RUN**, pois o sandbox é Linux
- probe real de `PS4_LAPY20014_v1.01.pkg`: **PASS** para metadata/catalogação da fase anterior
- configuração/compilação C++ completa do core: **BLOCKED** antes do build porque o ZIP recebido não contém todos os submódulos/dependências; veja `build_probe.log`
- boot do PKG PS4 no KytyPS5: **BLOCKED** por ser um core PS5 e por PKG bruto não ser alvo do CLI
- `PS4_LAPY20009_v2.07.pkg`: **NOT TESTED**, arquivo não fornecido nesta conversa

## Mudanças C++ desta rodada

Foram revisadas estaticamente e geradas em `performance_v2.patch`:

- persistência de `VkPipelineCache` em builds de arquivo/ZIP;
- índice XXH3 das especializações de shader;
- `VK_DYNAMIC_STATE_BLEND_CONSTANTS` + `vkCmdSetBlendConstants`;
- canonicalização de blend state inativo/separate-alpha irrelevante;
- hash XXH3 da parte estática da chave de pipeline;
- reservas dos hash maps e estatísticas de cache.

Sem o binário Windows recompilado e uma GPU Vulkan real, não há benchmark de FPS responsável a declarar. O objetivo destas mudanças é remover trabalho redundante e reduzir stutter/variantes, não prometer um número mágico num ambiente onde o executável nem pôde ser linkado.
