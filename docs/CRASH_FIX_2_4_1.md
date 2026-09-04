# HyperCore 2.4.1 — Windows loader crash fix

## Crash observado

O log de teste retornou `-1073741701`, que em 32 bits sem sinal é `0xC000007B` (`STATUS_INVALID_IMAGE_FORMAT`).
Esse status acontece **antes** de `main()` da engine e, portanto, antes de Vulkan, shaders, PM4 ou o ELF do jogo serem processados.

A versão 2.4 classificava o caso incorretamente como possível falha de shader porque o diagnóstico vasculhava o log completo do launcher; a própria linha de comando continha `--shader-optimization-type`, produzindo um falso positivo. Isso também habilitava o retry seguro sem qualquer chance de corrigir um erro do Windows loader.

## Correções

- Diagnóstico usa o **exit code primeiro** e mostra hexadecimal/NTSTATUS.
- Diagnóstico de shader analisa somente stdout/stderr real do core, não a linha de comando do launcher.
- `0xC000007B`, `0xC0000135`, `0xC0000142`, access violation e illegal instruction têm categorias separadas.
- Retry gráfico não é executado para falha de loader, DLL ausente, bad image, access violation ou instrução ilegal.
- Corrigido o estado de recovery para impedir múltiplos retries concorrentes e múltiplos boots sobrepostos.
- `engine_doctor.py` analisa PE/COFF, valida AMD64, enumera imports e detecta DLL local x86/ARM64 ao lado de uma engine x64.
- DLL importada de arquitetura errada pode ser movida automaticamente para `engines/ps5/_quarantine_wrong_arch` em vez de ser apagada.
- `1.bat` executa o doctor antes de abrir a UX.
- O bootstrap passa a preferir e copiar o **install tree inteiro** de `_Build/windows/install`, não apenas `kyty_emulator.exe` isolado.
- Se VC++ x64 estiver ausente, o runtime repair baixa somente o redistribuível oficial Microsoft e valida a assinatura antes de instalar.
- Se a engine ainda for incapaz de carregar, há fallback automático para uma release oficial KytyPS5 via GitHub Releases. A engine quebrada é preservada em `engines/backup/`.
- O preflight da UX roda o mesmo runtime doctor antes de iniciar um jogo.
- ELFs 32-bit, de arquitetura errada ou claramente Linux-host são rejeitados antes do boot.

## Por que isso importa

Nenhuma alteração de `Fifo`, readback, resolução, shader optimization ou VRAM pode resolver `0xC000007B`. O Windows não chegou a executar o código do emulador. O alvo correto é o binário/dependências da engine.

## Resultado esperado no próximo teste

Ao iniciar `1.bat`, o terminal deve mostrar algo semelhante a:

```
[ENGINE-DOCTOR] PE=x64/AMD64 imports=...
[ENGINE-DOCTOR] runtime Vulkan=OK | VC140=OK ...
[ENGINE-DOCTOR] probe --help: rc=0 ...
[ENGINE-DOCTOR] runtime da engine parece carregável.
```

Se aparecer novamente `0xC000007B`, o terminal agora mostrará qual DLL local tem arquitetura errada quando ela estiver entre os imports diretos. Se a engine em si estiver corrompida ou não for AMD64, o boot é bloqueado e o fallback de engine é acionado.
