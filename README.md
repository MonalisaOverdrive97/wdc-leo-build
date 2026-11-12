# World Driver Championship — recompilação para PC (Leo Build)

Recompilação estática **não oficial** do *World Driver Championship* (Nintendo 64, Boss Game Studios / Midway, 1999) para Windows, feita com
[N64Recomp](https://github.com/N64Recomp/N64Recomp) / N64ModernRuntime e o renderizador [parallel-RDP](https://github.com/Themaister/parallel-rdp) (Vulkan).
Projeto de fã, sem ligação com a Boss Game Studios, a Midway ou a Nintendo. **Esse projeto foi feito com auxílio de IA.**

> **Este repositório não contém a ROM nem o código do jogo original** (nem o C/ucode recompilados, nem disassemblies). Para jogar é preciso ter o seu próprio arquivo da ROM
> (versão dos EUA, "USA"), que o programa pede na primeira vez e guarda uma cópia.

## Baixar e jogar (Windows 10/11, 64 bits)
1. Na aba **Releases**, baixe `WorldDriverChampionship-LeoBuild-0.1.0-canary-win64.zip`.
2. Extraia a pasta, abra `WorldDriverChampionship.exe` e aperte **JOGAR**. Na primeira vez escolha o arquivo da ROM.
3. Leia o `LEIA-ME.txt` do pacote (controles, modos, saves, problemas comuns).

Requisitos: placa de vídeo com Vulkan 1.1 (NVIDIA/AMD/Intel recentes, driver atualizado), ~2 GB de memória livre. Não precisa instalar mais nada.

## O que tem
- **Dois modos**: *Legacy* (recompilação pura: 30 fps em corrida, resolução original) e *Enhanced* (60 fps, resolução interna 1×–4× ou automática, v-sync, filtros de imagem, filtro anti-piscada).
- **Setup** gráfico com **controles personalizáveis** (clássico/moderno) e **ajustes de imagem** (resolução, 4:3 ou esticada, suavização incluindo bicúbica, V-Sync, tela cheia).
- Controle XInput e teclado; Controller Pak virtual (saves, um por modo); atalho na Área de Trabalho; instalação portátil (uma pasta).

## Código-fonte (GPL-3.0)
O programa usa o **N64ModernRuntime (GPL-3.0)**; por isso o projeto é distribuído sob **GPL-3.0** (ver `LICENSE`) e o código-fonte do projeto está aqui:

| Pasta | Conteúdo |
|---|---|
| `recomp/project/` | O programa: `src/` (runtime, RDP em Vulkan, áudio, entrada, setup, apresentador D3D11), `CMakeLists.txt`, `assets/`, `test/` |
| `recomp/runtime_glue/`, `recomp/runtime_shim/` | Ligação com o runtime de N64 |
| `tools/recomp/` | Geradores (`gen_wdc_toml.py`…) e scripts de build |
| `tools/release/` | Build de distribuição e empacotamento (`build_release.cmd`, `make_package.py`) |
| `patches/` | Remendo do N64ModernRuntime (reservar 4 GiB de endereços, comitar só o necessário) |
| `recomp/ENHANCED_MODE.md`, `recomp/TESTES.md` | Documentação técnica e roteiro de testes |

**Não estão aqui** (derivam do jogo): a ROM, o C e o microcódigo do RSP recompilados, os disassemblies e as listas geradas. Para compilar é preciso a sua ROM, gerar esses arquivos
com as ferramentas (`tools/recomp/`) e ter o toolchain (Visual Studio Build Tools, LLVM/clang-cl, CMake, Ninja). Veja `recomp/ENHANCED_MODE.md` (§8–§10) e `tools/release/`.
O projeto também depende de N64Recomp, N64ModernRuntime e parallel-RDP, que **não** estão copiados aqui (usam o remendo de `patches/`).

## Avisos
- A tela do setup usa a arte do anúncio de revista do jogo (© Midway); o ícone deriva dela. É material de fã, sem fins comerciais.
- World Driver Championship pertence aos seus donos. Nenhum arquivo do jogo original é distribuído.
- parallel-RDP: MIT. N64Recomp: MIT. N64ModernRuntime: GPL-3.0.
