<div align="center">

<img src="https://capsule-render.vercel.app/api?type=waving&color=0:07111F,45:0F4C81,100:00A8FF&height=240&section=header&text=PKG_DoW&fontSize=68&fontColor=FFFFFF&animation=fadeIn&fontAlignY=34&desc=PS4%20%26%20PS5%20Emulation%20for%20Windows&descAlignY=57&descSize=20" width="100%" />

# 🎮 PS4 & PS5 Emulation. One Launcher. One Library.

### Execute conteúdo compatível de PlayStation 4 e PlayStation 5 no Windows com uma UX moderna, cores modulares e otimizações Vulkan focadas em frame time e desempenho.

<img src="https://img.shields.io/badge/PS4-Emulation-006FCD?style=for-the-badge&logo=playstation&logoColor=white" />
<img src="https://img.shields.io/badge/PS5-Emulation-003791?style=for-the-badge&logo=playstation&logoColor=white" />
<img src="https://img.shields.io/badge/Windows-10%20%7C%2011-0078D4?style=for-the-badge&logo=windows&logoColor=white" />
<img src="https://img.shields.io/badge/Vulkan-1.3-AC162C?style=for-the-badge&logo=vulkan&logoColor=white" />

<br><br>

**Game Library · PKG Import · PS4 · PS5 · Vulkan · AutoBoost · Hardware Diagnostics**

</div>

---

## O que é o PKG_DoW?

**PKG_DoW é uma plataforma de emulação de PS4 e PS5 para Windows**, construída em torno de uma interface desktop própria e de cores de emulação independentes.

Em vez de obrigar o usuário a lidar diretamente com executáveis, argumentos, pastas, runtimes e configurações diferentes, o PKG_DoW concentra o fluxo em uma única aplicação:

```text
Instalar → Importar jogo → Detectar plataforma → Escolher core → Jogar
```

A biblioteca identifica o conteúdo disponível, organiza os jogos e seleciona o ambiente PS4 ou PS5 correspondente.

O launcher também detecta o hardware instalado e configura automaticamente políticas de GPU, memória, cache e compilação para reduzir trabalho manual e melhorar a experiência durante a execução.

### Em resumo

**Não é apenas um frontend.**

O projeto combina:

* emulação de **PlayStation 4**;
* emulação de **PlayStation 5**;
* biblioteca unificada;
* importação de PKG;
* gerenciamento modular de cores;
* diagnóstico automático de hardware;
* configuração de Vulkan;
* gerenciamento de shader/pipeline cache;
* otimizações de frame time;
* perfis específicos por arquitetura de GPU;
* ambiente isolado e instalação automatizada.

> [!IMPORTANT]
> O PKG_DoW não distribui jogos, firmware, BIOS, chaves, contas ou qualquer conteúdo protegido.
>
> Utilize apenas conteúdo obtido legalmente.
>
> PKG_DoW é um projeto independente e não possui vínculo com Sony Interactive Entertainment, PlayStation, Kyty ou shadPS4.

---

# 🖥️ Emulação sem UX de ferramenta de laboratório

Emulador não precisa parecer uma janela de debug de 2007.

O PKG_DoW possui uma interface desktop para controlar biblioteca, importação, cores e diagnóstico sem exigir que o usuário configure manualmente cada componente.

As imagens abaixo são capturas reais da aplicação.

<table>
  <tr>
    <td width="50%">
      <img src="docs/screenshots/ux/home.png" alt="Tela inicial do PKG_DoW">
      <br>
      <b>Home</b><br>
      Visão geral do ambiente e acesso rápido aos jogos.
    </td>
    <td width="50%">
      <img src="docs/screenshots/ux/library.png" alt="Biblioteca de jogos do PKG_DoW">
      <br>
      <b>Game Library</b><br>
      Biblioteca unificada para conteúdo PS4 e PS5.
    </td>
  </tr>
  <tr>
    <td width="50%">
      <img src="docs/screenshots/ux/import.png" alt="Importação de PKG no PKG_DoW">
      <br>
      <b>PKG Import</b><br>
      Importação e identificação do conteúdo compatível.
    </td>
    <td width="50%">
      <img src="docs/screenshots/ux/system.png" alt="Diagnóstico de hardware do PKG_DoW">
      <br>
      <b>System Diagnostics</b><br>
      GPU, Vulkan, VRAM, memória, runtime e estado do ambiente.
    </td>
  </tr>
</table>

---

# 🚀 Feito para jogar, não para configurar

O fluxo normal não exige compilar o projeto nem montar manualmente um ambiente de emulação.

```text
                    ┌───────────────┐
                    │   PKG_DoW     │
                    │    Library    │
                    └───────┬───────┘
                            │
                    Detecta plataforma
                            │
                 ┌──────────┴──────────┐
                 │                     │
              PS4 Game              PS5 Game
                 │                     │
                 ▼                     ▼
            ┌─────────┐           ┌─────────┐
            │ PS4 Core│           │ PS5 Core│
            └────┬────┘           └────┬────┘
                 │                     │
                 └──────────┬──────────┘
                            ▼
                     Vulkan Backend
                            │
                            ▼
                           GPU
```

O launcher mantém cada core separado e baixa apenas os componentes necessários.

Isso significa que uma instalação destinada apenas a PS4 não precisa carregar todo o ambiente PS5, e vice-versa.

---

# ⚡ AutoBoost

## Menos stutter. Melhor utilização da GPU. Frame time mais consistente.

O caminho gráfico do projeto inclui alterações destinadas a reduzir alguns dos gargalos mais comuns encontrados durante emulação: compilação de pipelines, sincronização excessiva, uploads temporários, pressão de VRAM e overhead de driver.

### Principais otimizações

| Tecnologia                        | O que muda                                                                                                         |
| :-------------------------------- | :----------------------------------------------------------------------------------------------------------------- |
| ⚡ **Async Compute Multi-Queue**   | Compute pode avançar em fila dedicada quando a carga permite, reduzindo disputa desnecessária com trabalho gráfico |
| 🧵 **Parallel Pipeline Compiler** | Pipelines são compilados em background utilizando múltiplos workers                                                |
| ♻️ **Descriptor Cache**           | Estados equivalentes podem reutilizar descriptors em vez de gerar novas alocações                                  |
| 🚄 **Non-Blocking Upload Ring**   | Spill buffers evitam bloqueios quando a região principal de upload ainda está ocupada                              |
| 🔀 **Synchronization2**           | Barriers usam estágios e acessos mais específicos para evitar serialização desnecessária                           |
| 🧠 **Adaptive VRAM**              | `VK_EXT_memory_budget` permite adaptar políticas de memória à GPU instalada                                        |
| 💾 **Persistent Pipeline Cache**  | Pipelines compatíveis podem ser reutilizados entre execuções                                                       |
| 🔗 **Cross-Queue Wait Registry**  | Sincronização entre filas possui coordenação central e proteção contra deadlock                                    |
| 🎯 **GPU Architecture Profiles**  | Ampere, Ada e Blackwell recebem políticas automáticas específicas                                                  |

---

## O objetivo não é somente aumentar o FPS médio

Uma execução a 60 FPS com picos constantes de frame time ainda parece ruim.

Por isso o AutoBoost trabalha também em fontes de **stutter**:

```text
Pipeline compilation
        ↓
Parallel workers
        ↓
Persistent cache
        ↓
Menos recompilação
        ↓
Frame delivery mais consistente
```

Da mesma forma:

```text
VRAM disponível
      ↓
VK_EXT_memory_budget
      ↓
Pressure monitoring
      ↓
Residency policy
      ↓
Menos eviction / thrashing
```

O objetivo é melhorar tanto **throughput** quanto **consistência de frame time**.

---

# 🎯 PS4 + PS5 em uma biblioteca

O usuário não precisa manter dois launchers independentes.

O PKG_DoW identifica os metadados disponíveis e direciona o conteúdo para o core correspondente.

```text
                   GAME LIBRARY
                        │
          ┌─────────────┴─────────────┐
          │                           │
       PS4 Title                   PS5 Title
          │                           │
          ▼                           ▼
      PS4 Engine                  PS5 Engine
          │                           │
          └─────────────┬─────────────┘
                        │
                      Vulkan
                        │
                        ▼
                       GPU
```

Cada engine continua independente e pode ser instalado, atualizado ou substituído sem transformar o repositório inteiro em um download gigantesco.

---

# 🧠 Pipeline gráfico

O projeto não tenta vender a fantasia de que shaders de console são magicamente convertidos em “código NVIDIA”.

O caminho continua sendo:

```text
Prospero / RDNA2
       │
       ▼
    Decoder
       │
       ▼
       IR
       │
       ▼
 Optimizations
       │
       ▼
    SPIR-V
       │
       ▼
    Vulkan
       │
       ▼
   GPU Driver
       │
       ▼
 Native GPU ISA
```

A compilação final continua sendo responsabilidade do driver da GPU.

O trabalho do PKG_DoW é melhorar o caminho **antes e ao redor dessa etapa**, reduzindo recompilações, sincronizações desnecessárias e pressão de memória quando possível.

---

# 📈 E o FPS?

O projeto contém otimizações que **podem aumentar desempenho e melhorar frame time** quando a execução está limitada por pipeline compilation, sincronização, overhead de CPU/driver ou gerenciamento de memória.

O ganho real depende de:

* jogo;
* cena;
* GPU;
* CPU;
* driver;
* resolução;
* estado do shader cache;
* core utilizado.

> [!NOTE]
> Não existe um número universal do tipo “+40% FPS”.
>
> Qualquer percentual sem uma suíte A/B reproduzível seria marketing fantasiado de benchmark.

### Como comparar corretamente

Uma comparação séria deve usar:

```text
Mesmo jogo
Mesmo save
Mesma cena
Mesma resolução
Mesmo driver
Mesmo hardware
Mesmo estado de cache
Mesma duração
```

E publicar pelo menos:

| Métrica               | Por quê                    |
| :-------------------- | :------------------------- |
| **Average FPS**       | Throughput geral           |
| **1% Low**            | Quedas perceptíveis        |
| **0.1% Low**          | Stutters severos           |
| **Frame Time**        | Consistência real          |
| **VRAM Usage**        | Pressão de memória         |
| **Pipeline Compiles** | Custo de shaders/pipelines |

Até existirem resultados públicos reproduzíveis, o projeto prefere mostrar **engenharia verificável em vez de porcentagens inventadas**.

---

# 📦 Instalação

### Requisitos

* Windows 10 ou Windows 11 x64
* GPU com suporte Vulkan atualizado
* conexão com internet
* espaço disponível para os cores e jogos

### Instalação rápida

```text
1. Code → Download ZIP
2. Extraia o projeto
3. Execute Setup.cmd
4. Escolha PS4, PS5 ou ambos
5. Execute 1.bat
6. Importe seus jogos
```

O instalador cuida do restante.

### O Setup instala

* Python 3.10+ quando necessário;
* ambiente Python isolado;
* PyQt6;
* Qt WebEngine;
* core PS4 opcional;
* core PS5 opcional;
* dependências necessárias;
* validação de integridade dos pacotes suportados.

---

## PowerShell

Também é possível automatizar a instalação:

```powershell
powershell -ExecutionPolicy Bypass -File tools/install.ps1 -WithPS4 -WithPS5 -Launch
```

Flags:

```text
-WithPS4    instala suporte PS4
-WithPS5    instala suporte PS5
-Launch     abre o PKG_DoW após instalação
-Force      recria componentes locais
```

---

# 🪶 Modular por design

O Git contém código.

Não precisa conter vários gigabytes de runtimes, builds e binários que o usuário talvez nunca utilize.

```text
Git clone
   │
   ▼
Setup.cmd
   │
   ├── .runtime/
   │
   ├── engines/PS4
   │
   ├── engines/PS5
   │
   └── userdata/
   │
   ▼
PKG_DoW
```

Você baixa apenas os componentes necessários.

---

# 📁 Estrutura

### Versionado no Git

```text
PKG_DoW/
├── Setup.cmd
├── 1.bat
├── webui/
├── tools/
├── src/
├── tests/
├── 3rdparty/
└── .github/
```

### Gerado localmente

```text
.runtime/     Python + dependências
engines/      cores PS4 / PS5
userdata/     biblioteca do usuário
logs/         diagnóstico e execução
_Build/       builds locais
```

Nenhum jogo deve ser armazenado no histórico Git.

---

# 🔧 Hardware Diagnostics

Antes de iniciar um jogo, o PKG_DoW consegue verificar o ambiente disponível.

Entre as informações analisadas:

```text
✓ GPU detectada
✓ Arquitetura da GPU
✓ Vulkan disponível
✓ Versão do driver
✓ VRAM disponível
✓ RAM do sistema
✓ Espaço em disco
✓ Runtime do Windows
✓ Core PS4
✓ Core PS5
✓ Pipeline cache
```

Isso permite encontrar problemas **antes** de descobrir que alguma coisa está errada depois de quinze minutos olhando para uma tela preta, tradição ancestral da emulação.

---

# 🛠️ Desenvolvimento

Clone o projeto com as dependências C++:

```powershell
git clone --recurse-submodules URL_DO_REPOSITORIO
cd PKG_DoW
```

### Testes Python

```powershell
python tools/test_pkg_dow_server.py
python tools/test_engine_doctor.py
```

### Build Windows

Requer:

* CMake
* Ninja
* Visual Studio Build Tools 2022
* `clang-cl`
* Qt 6
* Vulkan SDK

```powershell
cmake -S . -B _Build/windows -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=clang-cl `
  -DCMAKE_CXX_COMPILER=clang-cl `
  -DCMAKE_PREFIX_PATH="C:/Qt/6.x.x/msvc2022_64"

cmake --build _Build/windows --target launcher --parallel
cmake --install _Build/windows --prefix _Build/windows/install
```

CI compila Windows, Linux e macOS.

Artefatos compilados pertencem às releases, não ao histórico Git.

---

# 🔒 Segurança e privacidade

* servidor local limitado a `127.0.0.1`;
* nenhuma conta é necessária;
* nenhuma telemetria é enviada pelo launcher;
* componentes PS4 suportados possuem verificação SHA-256;
* jogos, firmware e chaves não são distribuídos;
* engines e runtimes ficam separados do código-fonte.

Encontrou um problema?

Ao abrir uma issue, inclua:

```text
Sistema operacional
CPU
GPU
Versão do driver
Core utilizado
Jogo / serial quando aplicável
Log completo
```

**Nunca envie jogos, firmware ou chaves.**

---

# ⚖️ Licença

PKG_DoW é distribuído sob **GPL-2.0**.

Dependências de terceiros mantêm suas próprias licenças em `LICENSES/` e nos respectivos projetos.

---

<div align="center">

## 🎮 PS4. PS5. Windows.

### Uma biblioteca. Cores modulares. Vulkan otimizado.

**Instale → Importe → Jogue**

<br>

<img src="https://img.shields.io/badge/PS4-READY-006FCD?style=for-the-badge&logo=playstation&logoColor=white" />
<img src="https://img.shields.io/badge/PS5-READY-003791?style=for-the-badge&logo=playstation&logoColor=white" />
<img src="https://img.shields.io/badge/AutoBoost-ENABLED-00A8FF?style=for-the-badge" />

<br><br>

**Windows · PS4 · PS5 · Vulkan · Qt · Python**

<img src="https://capsule-render.vercel.app/api?type=waving&color=0:00A8FF,50:0F4C81,100:07111F&height=130&section=footer" width="100%" />

</div>
