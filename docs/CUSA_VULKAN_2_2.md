# PKG_DoW 2.2 — integração CUSA + Vulkan

## Escopo

Esta fase não tenta transformar o core PS5 em um emulador PS4 por decreto administrativo. CUSA é tratado na camada de launcher e despachado para um `shadPS4` fornecido pelo usuário, enquanto o KytyPS5 continua responsável por PS5. O objetivo foi reduzir erros de roteamento, escolher melhor a GPU Vulkan e tornar o diagnóstico útil antes do boot.

## CUSA

O backend Python agora normaliza e indexa IDs `CUSAxxxxx`, lê campos de `param.sfo` e mantém por título:

- `TITLE_ID` / CUSA;
- `CONTENT_ID`;
- `APP_VER` / `VERSION`;
- `SYSTEM_VER` quando presente;
- categoria e região inferida do prefixo do Content ID;
- perfil de desempenho e modo de boot PS4 independentes por jogo.

Foi adicionado um scanner de raiz CUSA. Ele procura pastas que contenham `sce_sys/param.sfo` ou `eboot.bin`, cadastra o título e evita descer em árvores de cache/dados depois que encontra uma raiz de jogo.

A identidade de biblioteca de um título CUSA agora deriva do próprio serial (`ps4:CUSAxxxxx`) em vez do caminho absoluto. Assim, mover a pasta do jogo e fazer novo scan atualiza a entrada existente, em vez de criar um clone só porque o usuário organizou o disco.

### Modos de boot PS4

**Auto / eboot** é o padrão. Ele envia o `eboot.bin` diretamente ao shadPS4 e não depende da lista interna de pastas do shadPS4.

**CUSA** envia o serial como argumento, por exemplo `CUSA00001`. Esse modo é útil quando o usuário já cadastrou a raiz de jogos no shadPS4. O README oficial do shadPS4 documenta esse formato de inicialização.

O launcher também executa `shadPS4 --help` e descobre dinamicamente opções disponíveis. Flags como fullscreen, GPU e present mode só são adicionadas quando a versão instalada realmente as anuncia. Isso evita quebrar o boot quando a CLI muda entre releases.

## Vulkan no launcher

O backend Python ganhou um probe Vulkan nativo usando apenas `ctypes`:

1. carrega `vulkan-1.dll`, `libvulkan.so` ou equivalente;
2. consulta a versão do loader;
3. cria uma instância mínima;
4. enumera adaptadores físicos;
5. coleta nome, tipo, API Vulkan, IDs PCI/vendor e memória device-local;
6. enumera extensões do dispositivo;
7. marca se o adaptador atende Vulkan 1.3 + `VK_KHR_swapchain` + `VK_KHR_push_descriptor` para o caminho PS4/shadPS4.

A UX usa essa enumeração para preencher o seletor de GPU em vez de pedir que o usuário adivinhe um número. A humanidade já inventou menus suspensos; podemos ao menos aproveitar essa rara vitória.

O fallback PowerShell preserva a mesma API da UX. Sem Python ele detecta o loader e, quando `vulkaninfo.exe` está disponível, também extrai adaptadores do resumo do utilitário.

## Vulkan no core C++

`src/graphics/presentation/window/vulkanWindow.cpp` recebeu duas correções de seleção de adaptador:

- **índice configurado com bounds check**: um `--gpu` fora da faixa não indexa mais o vetor de dispositivos diretamente; registra o erro e volta para seleção automática;
- **score de seleção automática**: depois de validar features, extensões, formatos, queue/present e surface capabilities, candidatos são comparados por classe de dispositivo (dedicada > integrada > virtual > CPU) e, dentro da mesma classe, memória device-local. Antes, uma GPU dedicada posterior simplesmente substituía outra dedicada anterior.

Isto não promete FPS por si só, mas evita um caso bastante real de usar a GPU errada em máquinas híbridas ou multi-GPU.

## Sincronização

O core já exige Vulkan 1.3, `dynamicRendering`, `synchronization2` e timeline semaphores. Não converti o submit principal para `vkQueueSubmit2` nesta fase porque isso muda semântica de sincronização e precisa de captura/validação numa GPU real. Vulkan deixa sincronização e visibilidade de memória explicitamente sob responsabilidade da aplicação, então trocar primitivas sem medir é uma maneira muito sofisticada de criar flicker.

## Fontes técnicas consultadas

- shadPS4 README / uso por CUSA: https://github.com/shadps4-emu/shadPS4/
- shadPS4 settings / Vulkan GPU e cache: https://github.com/shadps4-emu/shadPS4/blob/main/src/core/emulator_settings.h
- shadPS4 quick start / requisitos Vulkan: https://github.com/shadps4-emu/shadPS4/wiki/I.-Quick-start-%5BUsers%5D
- Vulkan synchronization: https://docs.vulkan.org/spec/latest/chapters/synchronization.html
- Vulkan timeline semaphores: https://docs.vulkan.org/tutorial/latest/Advanced_Vulkan_Compute/08_Asynchronous_Compute/03_timeline_semaphores.html

## Validação realizada

- `python -m py_compile tools/pkg_dow_server.py`;
- `python tools/test_pkg_dow_server.py`, incluindo fixture CUSA e descoberta de flags de CLI;
- `node --check webui/app.js`;
- verificação automática de IDs usados pelo JS contra o HTML;
- servidor HTTP real com `/`, `/api/status` e `/api/vulkan`;
- tentativa de configuração CMake registrada em `docs/build_2_2_configure.log`.

O CMake parou ao tentar baixar `xbyak` porque este sandbox não possui resolução de rede para o subprocesso do Git. Portanto o patch C++ foi revisado e mantido no fonte, mas não foi compilado aqui. Essa limitação está registrada, em vez de ser magicamente convertida em “100% testado”.
