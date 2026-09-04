# Relatório de teste dos PKGs

## PS4_LAPY20014_v1.01.pkg

- Presente no ambiente de análise: sim.
- Tamanho: 57.475.072 bytes.
- SHA-256: `5e689636fefe29d1b003dda917b7b07631decefe869d7ec8d9770bd5848c3e2f`.
- Magic: `7f 43 4e 54`.
- Content ID: `VR1234-LAPY20014_00-0000000000000000`.
- Title ID: `LAPY20014`.
- Título detectado: `PS4 Player 3D`.
- Classificação: PS4 PKG.
- Probe de metadata do launcher: **PASS**.
- Boot por KytyPS5 recebido: **BLOCKED / não suportado**. O CLI requer pasta/ELF e o runtime linker rejeita executável principal PS4 não-next-gen.
- Boot por shadPS4: **não executado**. Nenhum binário shadPS4 Windows foi fornecido, e o sandbox não é Windows.

## PS4_LAPY20009_v2.07.pkg

- Presente no ambiente de análise: **não**.
- Status: **NOT TESTED**.
- Motivo: o arquivo não foi anexado/montado. Nenhum dado de hash, metadata ou execução é inferido.

## Política do launcher

A UX não tenta "executar" PKG bruto fingindo sucesso. Ela cataloga o arquivo e, para PS4, orienta a instalação pelo core PS4 configurado. Pastas/ELFs extraídos podem ser despachados diretamente. Isso evita logs falsamente verdes que só adiam o crash para cinco segundos depois.
