# World Driver Championship — recompilação para PC (Leo Build)

Recompilação estática **não oficial** do *World Driver Championship* (Nintendo 64, Boss Game Studios / Midway, 1999) para Windows, feita com
[N64Recomp](https://github.com/N64Recomp/N64Recomp) / N64ModernRuntime e o renderizador [parallel-RDP](https://github.com/Themaister/parallel-rdp) (Vulkan).
Projeto de fã, sem ligação com a Boss Game Studios, a Midway ou a Nintendo. **Esse projeto foi feito com auxílio de IA.**

> **Este repositório não contém a ROM nem o código do jogo original** (nem o C/ucode recompilados, nem disassemblies, nem executáveis). Para jogar é preciso ter o seu próprio arquivo
> da ROM (versão dos EUA, "USA"), que o programa pede na primeira vez e guarda uma cópia. A tela do setup e o ícone usam a arte do anúncio de revista do jogo (© Midway), como material de fã.

## O que tem
- **Dois modos**: *Legacy* (recompilação pura: 30 fps em corrida, resolução original) e *Enhanced* (60 fps, resolução interna 1×–4× ou automática, v-sync, filtros de imagem, filtro anti-piscada).
- **Setup** gráfico com **controles personalizáveis** (clássico/moderno) e **ajustes de imagem** (resolução, 4:3 ou esticada, suavização incluindo bicúbica, V-Sync, tela cheia).
- Controle XInput e teclado; Controller Pak virtual (saves, um por modo); atalho na Área de Trabalho; instalação portátil (uma pasta).

O executável pronto **não é distribuído aqui**: ele contém o código do jogo recompilado. Quem quiser jogar compila a partir deste código com a própria ROM (abaixo).

## Código-fonte (GPL-3.0)
O programa usa o **N64ModernRuntime (GPL-3.0)**; por isso o projeto é distribuído sob **GPL-3.0** (ver `LICENSE`):

| Pasta | Conteúdo |
|---|---|
| `recomp/project/` | O programa: `src/` (runtime, RDP em Vulkan, áudio, entrada, setup, apresentador D3D11), `CMakeLists.txt`, `assets/`, `test/` |
| `recomp/runtime_glue/`, `recomp/runtime_shim/` | Ligação com o runtime de N64 |
| `tools/recomp/` | Geradores (`gen_wdc_toml.py`…) e scripts de build |
| `tools/release/` | Build de distribuição e empacotamento (`build_release.cmd`, `make_package.py`) |
| `patches/` | Remendos das dependências (ver abaixo) |
| `recomp/ENHANCED_MODE.md`, `recomp/TESTES.md` | Documentação técnica e roteiro de testes |

### Dependências e versões exatas (não copiadas aqui)
| Projeto | Repositório | Commit usado | Licença |
|---|---|---|---|
| N64ModernRuntime (ultramodern + librecomp) | https://github.com/N64Recomp/N64ModernRuntime | `cdf5abbd5026fef5c364c676e4667c45e42b6863` + `patches/N64ModernRuntime-reserve-not-commit.patch` | GPL-3.0 |
| N64Recomp (submódulo do runtime) | https://github.com/N64Recomp/N64Recomp | `81213c1831fab2521a6a5459c67b63437d67e253` | MIT |
| N64Recomp (ferramentas RSPRecomp) | https://github.com/N64Recomp/N64Recomp | `ffb39cdad1da5de07eaaa48bd1db4a89a7986771` + `patches/N64Recomp-RSPRecomp-wdc.patch` | MIT |
| parallel-RDP (standalone) | https://github.com/Themaister/parallel-rdp-standalone | `388d70f5835b352d841d9d9e5a08c5de01470f41` + `patches/parallel-rdp-point-sampling.patch` | MIT |

**Não estão aqui** (derivam do jogo): a ROM, o C e o microcódigo do RSP recompilados, os disassemblies e as listas geradas. Para compilar é preciso a sua ROM, gerar esses arquivos
com as ferramentas (`tools/recomp/`) e ter o toolchain (Visual Studio Build Tools, LLVM/clang-cl, CMake, Ninja). Veja `recomp/ENHANCED_MODE.md` (§8–§10) e `tools/release/`.

## Avisos
- World Driver Championship pertence aos seus donos. Nenhum arquivo do jogo original é distribuído.
- Se você é titular de direitos e quer a remoção de algo, abra uma *issue* e será atendido.
