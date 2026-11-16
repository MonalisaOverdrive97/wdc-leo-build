# Modos Legacy e Enhanced — arquitetura e levantamento de melhorias

> **Estado (2026-10-28): M0 e M1 implementados** — ver §8 (como usar, configuração, medições). O restante deste documento é o levantamento original.

Escrito em 2026-10-28. Marcações de confiança: **[verificado]** = medido/lido nesta base de código ou em execução; **[indício]** = há evidência parcial; **[hipótese]** = ainda não testado. Esforço: S (horas–1 dia), M (dias), L (1–3 semanas), XL (semanas–meses, incerto). Esforço é relativo, não é prazo.

## 1. Os dois modos

| | **Legacy** | **Enhanced** |
|---|---|---|
| Objetivo | O jogo como rodaria no N64: mesma lógica, mesma taxa (30 Hz em corrida, 60 Hz nos menus), mesma resolução | Mesmo jogo, com melhorias opcionais (60 fps, resolução interna, widescreen, v-sync, …) |
| Critério de aceite | Passa o diferencial de CPU (82,1% dos bytes de código verificados) e as rotas R1–R15 sem divergência | Smoke tests próprios (ciclo intro→título→demo, Time Attack, 2P, save/load) + lista de divergências **esperadas** em relação ao Legacy |
| Padrão | sim (referência de correção) | opcional por perfil |

### 1.1 Como implementar sem duplicar tudo (recomendação)

Hoje o 60 fps existe como **patches de instrução em tempo de geração** (`gen_wdc_toml.py --exp60` → `RecompiledFuncs60`, um build à parte). Isso obriga a manter dois C gerados e dois builds. Proposta: **um único binário** e **um único C gerado**, com cada alteração virando **hook condicional** em tempo de execução:

- `wdc_enh` (estrutura global, preenchida de `wdc_config.toml` + `--mode legacy|enhanced`) com uma chave por recurso: `fps60`, `internal_scale`, `widescreen`, `vsync`, `draw_distance`, …
- Cada ponto de patch vira `if (wdc_enh.fps60) { … } else { original }`. Hoje são só 6 pontos do 60 fps (spec §21): `func_800013DC` 0x800015BC/0x800015D4 (2ª espera de VI), `func_800369E0` 0x80037B44 (divisor `n`), `func_8002C500` 0x8002C584 (divisor/período por modo), `func_80003DA4` (gravação a 30 Hz). Onde o patch é uma palavra de instrução (ex.: `addiu $a1,$zero,1`), o equivalente condicional é um hook na **entrada da função chamada** que reescreve o argumento (ex.: `func_8002CB0C`: `if (enh.fps60 && chamada de corrida) ctx->r5 = 0`).
- Vantagem: o Legacy continua sendo exatamente o C já validado (nenhum hook ativo ⇒ comportamento idêntico); o diferencial roda uma vez só. Custo: ~1 dia para converter os 6 pontos + testes.
- Alternativa (mais simples agora, pior a longo prazo): dois binários. Não recomendo.

### 1.2 Compatibilidade de dados (ponto fácil de esquecer)

Replays, demos e o fantasma do Time Attack são **re-simulações** de amostras de entrada gravadas a 30 Hz (spec §21.1). Recordes (tempos) e replays gravados em Enhanced podem **não** reproduzir igual no Legacy (medi 20,91 s a 60 Hz contra 20,83 s a 30 Hz na mesma rota). Recomendo **Controller Pak virtual separado por modo** (ex.: `pak_legacy.mpk`, `pak_enhanced.mpk`) para o ranking e os replays não se misturarem. [indício]

## 2. Já provado nesta base

| Item | Estado |
|---|---|
| 60 fps em corrida | **Funciona** [verificado]: 60 tarefas gráficas/s medidas; Time Attack ao vivo com roteiro idêntico fecha a volta em 20,91 s (60 Hz) vs 20,83 s (30 Hz), 094 vs 095 kph. O `dt` do jogo é nominal (período por retraço × `n+1`), não medido. Demo/replay voltam a 30 Hz para manter a re-simulação idêntica. **Falta**: teste de direção por humano, replay gravado ao vivo, fantasma. |
| Resolução interna | `WDC_RDP_SCALE=2|4|8` (upscaling do ParaLLEl-RDP) **funciona** [verificado]: 4× = 2560×960 nítido e **60 tarefas/s mantidas** (RTX 4050). **Falta**: retrovisor, efeitos que leem o framebuffer de volta, menus 2D, 8×. |
| Congelamento em tela cheia | Corrigido (spec §21.2): a pintura GDI segurava a thread do RDP. |

## 3. Os três itens pedidos

### 3.1 Resolução interna

**O que já temos** [verificado]: o ParaLLEl-RDP renderiza a geometria em 2×/4×/8× (flags `COMMAND_PROCESSOR_FLAG_UPSCALING_*_BIT`, `wdc_rdp_vk.cpp`). As texturas continuam nativas (a nitidez vem das bordas e da geometria).

**Opções do scanout já disponíveis no ParaLLEl** [verificado em `video_interface.hpp`]: `vi.aa`, `vi.scale`, `vi.serrate`, `vi.dither_filter`, `vi.divot_filter`, `vi.gamma_dither`, `upscale_deinterlacing` (já `true`), `crop_rect`, `downscale_steps`, e as flags `SUPER_SAMPLED_READ_BACK_BIT` / `SUPER_SAMPLED_DITHER_BIT`. Hoje usamos o padrão. Cada uma vira uma chave do Enhanced (ex.: desligar dither e divot na imagem upscalada).

**Trabalho**: expor `internal_scale = 1|2|4|8` e as chaves de VI na configuração (S); testar retrovisor, readback e menus (M); medir custo de GPU em 8× (S). **Risco**: efeitos que o jogo lê de volta do framebuffer (a verificar); a opção `SUPER_SAMPLED_READ_BACK` existe justamente para isso.

**Texturas** (HD): ver §4, item B4 — é outro problema (XL).

### 3.2 Widescreen

**Resposta curta**: viável, mas é a melhoria mais cara da lista (L–XL). O que sei, e o que falta:

**Como o jogo desenha** [verificado, spec §2–§6]:
1. A CPU monta matrizes 4×4 em ponto fixo 16.16 e as entrega ao ucode (comandos `04`/`08`/`0A`).
2. O ucode (`0C`) transforma vértices, faz a divisão perspectiva e gera **códigos de clipping**; `10` emite os triângulos RDP.
3. A CPU ainda faz **recorte e ordenação por profundidade** dos polígonos (`func_8002703C`, recortador `func_80026208`).
4. Os vértices saem com coordenadas de tela saturadas em x∈[−5440, 6719], y∈[−4320, 5279] (valores vistos na época do defeito do sqrt, spec §11.2 item 5; são os limites de saturação do ucode, mas a leitura como quartos de pixel, ±~1500 px ⇒ folga para 853 px de largura, é só **[indício]** e precisa ser conferida).

**O que o jogo guarda sobre a tela** [verificado na RAM, intro a 20 s]: uma **tabela de descritores de modo de tela** em `0x800AAE20…` (6 entradas de `0x74` bytes) com `320`, `240`, `640`, os campos de temporização do VI (`0x20D`, `0xC15`, …) e um campo **`1.333` (proporção 4:3)** por entrada. Existem também constantes `320.0`, `160.0`, `120.0`, `240.0` na RAM. Ou seja: a proporção **é um parâmetro** do jogo, mas **não localizei ainda quem o consome** [hipótese: a câmera/projeção].

**O que o widescreen exige** (cada item é um ponto de trabalho):
| # | Mudança | Onde | Esforço | Confiança |
|---|---|---|---|---|
| a | **FOV horizontal** (Hor+): escalar a linha X da matriz de projeção por `(4/3)/(16/9)` | código da CPU que compõe a matriz de projeção (localizar por rastreamento de leitura do campo `1.333`) | M | hipótese |
| b | **Viewport em X**: largura de tela 640→853 px (campo de 640×240) | também na matriz (a escala de tela está embutida, spec §6) | M | indício |
| c | **Framebuffer e scissor mais largos**: reescrever `SET_COLOR_IMAGE` (largura) e `SET_SCISSOR` na lista RDP antes de `enqueue_command`, e a largura/escala X do VI no scanout | `wdc_rdp_vk.cpp` (vejo todas as palavras RDP antes de enviá-las) | M–L | indício |
| d | **Memória do framebuffer largo**: o jogo vê 4 MB, mas **o RDP recebe 8 MB de RDRAM host** (`main.cpp:515`) ⇒ há 4 MB livres acima do que o jogo enxerga para framebuffers maiores [verificado] | mapear os color images para esse espaço | S–M | indício |
| e | **HUD/2D ancorado**: velocímetro, minimapa, textos e menus assumem 320 px; precisam de âncora (esquerda/centro/direita) ou ficam centralizados em 4:3 com barras | comandos 2D (ucode/CPU) | L | hipótese |
| f | **Culling/visibilidade**: se a visibilidade de objetos for por setor de pista, as bordas laterais podem mostrar "pop-in" | CPU (listas de objetos) | M–L | hipótese |
| g | **Split-screen 2P**, fundos/céu, retrovisor (HI-RES/NORM) | vários | L | hipótese |

**Plano de prova** (primeiro passo, S): marcar o campo `1.333` com um *watchpoint* de leitura (hook em `MEM_W` não existe; usar o recorder de segmentos ou um build com leitura instrumentada) para achar o(s) consumidor(es); depois uma **prova de conceito em 1P** (a+b+c+d) com HUD simplesmente centralizado em 4:3. Só então decidir sobre (e)–(g).

**Alternativa barata**: "widescreen por esticamento" (rodar em 4:3 e esticar a imagem) **não é recomendado** (distorce); "pillarbox" (barras) já é o que a janela faz.

### 3.3 V-sync e ritmo de quadros

**Situação atual** [verificado]:
- O ritmo do jogo vem da **thread de VI do ultramodern**: dorme até `start + vis·(1 s/60)` (`events.cpp`), ou seja, **60,000 Hz de relógio de parede**, **independente do monitor**. O runtime tem ganchos para taxa de quadros alvo e taxa de atualização da tela (`get_target_framerate`, `display_refresh_rate`), que este projeto **não usa**.
- A apresentação é **GDI**: o RDP entrega o quadro e a thread da janela faz `StretchDIBits` em `WM_PAINT`. Não há v-sync nem controle de apresentação; o DWM compõe quando quer.
- A tela do usuário (Intel, painel 2560×1600) está a **120 Hz** [verificado via `Win32_VideoController`]. Um relógio de 60,000 Hz contra um painel de 120 Hz (ou 144/165 Hz em outras máquinas) produz **batimento**: quadros ocasionalmente apresentados 2× ou pulados ⇒ "engasgos" sutis.
- Em tela cheia 2560×1600 a pintura GDI leva **41–52 ms** ⇒ ~20 quadros/s na tela mesmo com o jogo a 60.

**Proposta**: um **apresentador em GPU** (swapchain Vulkan ou D3D11/12 com `Present(1)`), que também faz o escalonamento da imagem (filtros: nearest, bilinear, *sharp-bilinear*, inteiro, opcional FSR1) e substitui o GDI. Duas políticas de ritmo:
1. **Travado no monitor** (padrão): o apresentador é o **relógio mestre**. A thread de VI deixa de dormir por tempo e passa a ser liberada a cada vblank (ou a cada *N* vblanks: 120 Hz ⇒ 1 quadro de jogo a cada 2). Sem batimento. Em monitores não múltiplos de 60 (ex.: 144 Hz) o ritmo exato não existe: usar VRR/G-Sync se houver, ou aceitar o padrão 3:2 (judder leve), ou deixar o usuário escolher "taxa do jogo".
2. **Livre** (para testes/benchmarks): comportamento atual.

**Trabalho**: apresentador + relógio mestre (M–L); integração com `ultramodern::get_target_framerate/display_refresh_rate` (S); fila de quadros com latência mínima (S–M). **Benefícios colaterais**: resolve a tela cheia a ~20 fps, abre filtros/shaders de CRT/escala, HDR e escolha de monitor.

## 4. Mais melhorias que o jogo pode receber

Valor: ★ baixo … ★★★ alto. Cada linha vale para o Enhanced (chave individual).

### Visual
| ID | Melhoria | O que exige | Esforço | Valor | Conf. |
|---|---|---|---|---|---|
| B1 | **Apresentador GPU + escalonamento/filtros** (§3.3) | novo módulo de apresentação | M–L | ★★★ | indício |
| B2 | **Opções de VI** (dither/divot/AA/gamma, deinterlace) por configuração | `ScanoutOptions` | S | ★★ | verificado |
| B3 | **Filtragem de textura** melhor que o 3 pontos do N64 | o RDP do ParaLLEl é bit-exato: exigiria editar shaders (embutidos em `slangmosh.hpp`) | L | ★★ | hipótese |
| B4 | **Texturas HD (substituição)** | interceptar `LOADBLOCK/SETTILE/SETTIMG` (passam sem alteração pelo ucode, spec §3), calcular hash, trocar por textura host; exige suporte no renderizador (ParaLLEl não tem; RT64 tem) | XL | ★★★ | hipótese |
| B5 | **Distância de visão / LOD / pop-in** | achar as constantes de corte e de LOD na CPU; testar limites de buffers do ucode | M–L | ★★ | hipótese |
| B6 | **Névoa (fog)** ajustável | cores/parâmetros de fog (`F8`) passam pelas listas RDP (spec §3) | S–M | ★ | indício |
| B7 | **HUD**: escala, posição, ocultar, km/h↔mph | comandos 2D + dados | M | ★★ | hipótese |
| B8 | **Câmera**: slider de FOV, distância; **modo foto** no replay | matriz de câmera; replays já são re-simulações (spec §21.1) | M | ★★ | hipótese |
| B9 | **Interpolação para 120+ fps** | exigiria HLE completo do 3D (RT64-style); os comandos `0C`/`10` e os 16 bytes de saída ainda não estão fechados (spec §4) | XL | ★★ | hipótese — **não recomendo** no início; v-sync a 120 Hz com 60 fps já dá fluidez |
| B10 | Cor 32 bits (anti-banding) | o jogo usa framebuffer 16 bits (RGBA5551) [verificado no código do `Fb`]; trocar exigiria mudar blender/formatos | L–XL | ★ | hipótese |

### Controles e jogabilidade
| ID | Melhoria | O que exige | Esforço | Valor | Conf. |
|---|---|---|---|---|---|
| C1 | **Vibração (Rumble Pak)** → XInput | o jogo chama `osMotorInit` (3 pontos) [verificado]; ligar `osMotorStart/Stop` a `XInputSetState` | S | ★★ | indício |
| C2 | **Zona morta/curvas do analógico**, sensibilidade da direção | hoje: zona morta radial de 20% reescalada (`wdc_input.cpp`) [verificado]; o jogo converte o eixo em `func_800031BC` | S | ★★ | verificado |
| C3 | **Remapeamento de teclas/botões** e tela de controles | UI + arquivo | M | ★★★ | — |
| C4 | **Menos latência de entrada** | 60 Hz já corta pela metade; medir e reduzir filas de quadros | S–M | ★★ | hipótese |
| C5 | **Save states** (instantâneo da RAM) e *quick save* | a RDRAM é uma região contígua [verificado: `--dump-ram`]; exige também estado de threads/filas ⇒ **difícil** num recompilado; alternativa: só salvar o Controller Pak | XL (states) / S (pak) | ★★ | hipótese |
| C6 | **Menu de cheats** (os 22 códigos da ROM, spec §20.2) e "liberar tudo" | a tabela existe e foi executada [verificado]; só falta UI | S | ★★ | verificado |
| C7 | **Pular logos/intro** | patch de dados/sequência | S | ★ | hipótese |
| C8 | **Fantasma/tempos**: múltiplas voltas, exportar tempos | dados do pak | M | ★ | hipótese |
| C9 | Dificuldade/IA ajustável | procurar parâmetros do "catch up" (já há opção no menu 2P) | M | ★★ | hipótese |

### Áudio
| ID | Melhoria | O que exige | Esforço | Valor | Conf. |
|---|---|---|---|---|---|
| A1 | **Reamostragem de qualidade** 22 kHz → 48 kHz | hoje: **interpolação linear** (`wdc_audio.cpp`) [verificado]; trocar por filtro sinc/polifásico | S–M | ★★ | verificado |
| A2 | Latência menor (WASAPI orientado a eventos) | já usa WASAPI | S–M | ★ | indício |
| A3 | Mixer de volumes por canal (música/efeitos) | o jogo já tem volumes no menu; expor em UI | S | ★ | — |

### Plataforma e manutenção
| ID | Melhoria | O que exige | Esforço | Valor |
|---|---|---|---|---|
| P1 | **Menu de configuração** (modo, escala, v-sync, controles) — overlay no jogo ou janela separada | UI (Dear ImGui é o mais direto) | M | ★★★ |
| P2 | **Perfis**: Pak/saves por modo (§1.2) | gerenciamento de arquivos | S | ★★★ |
| P3 | **Sistema de mods** (o runtime tem; `mod_game_id` está vazio) | ligar o suporte a mods do `librecomp` + TOML de patches | M | ★★ |
| P4 | **Regressão automática**: Legacy = diferencial de CPU; Enhanced = smoke tests por rota (R1–R15 já existem) com tolerâncias | scripts em `recomp/explore` | M | ★★★ |
| P5 | Portabilidade (Linux/Steam Deck): hoje depende de WASAPI e GDI | com o apresentador (B1) e áudio abstrato | L | ★ |

## 5. Ordem recomendada

| Marco | Entrega | Esforço | Por quê nesta ordem |
|---|---|---|---|
| M0 | Arquitetura de modos: `wdc_enh` + `--mode` + `wdc_config.toml`; 6 patches do 60 fps viram hooks condicionais; Pak por modo (P2) | M | Base de tudo; nada se perde do Legacy |
| M1 | **Apresentador GPU + v-sync + escala** (B1, §3.3) | M–L | Resolve a tela cheia a ~20 fps, remove o batimento 60/120 Hz e destrava as melhorias visuais |
| M2 | Opções de resolução interna e de VI na configuração (§3.1, B2) | S | Pronto para expor: o código já existe |
| M3 | Controles: rumble, curvas, remapeamento (C1–C3); áudio (A1); cheats (C6) | S–M | Ganhos grandes, risco baixo |
| M4 | **Widescreen** em fases: (i) achar o consumidor do `1.333`; (ii) prova de conceito 1P; (iii) HUD/2P/menus | L–XL | Alto valor, alto risco; só depois do apresentador (precisa de scanout largo) |
| M5 | LOD/distância de visão (B5), HUD (B7), câmera/foto (B8) | M cada | Opcionais, dependem de achar constantes |
| M6 | Texturas HD (B4) e/ou interpolação (B9) | XL | Só se M0–M5 estiverem estáveis; exigem decisão de arquitetura (renderizador próprio ou RT64) |

## 6. Riscos e decisões em aberto

1. **Física a 60 Hz**: o único teste ao vivo foi uma rota de Time Attack (tempo +0,4%). A sensação de direção precisa do teste de um humano; qualquer constante por iteração escondida aparece aí.
2. **Replays e recordes** não são portáveis entre modos (§1.2).
3. **Widescreen** depende de achar o consumidor do `1.333` e da prova de conceito antes de prometer 16:9 completo.
4. **Cobertura**: 18% dos bytes de código nunca executaram nas nossas rotas (290 funções); patches do Enhanced em código não verificado têm risco maior.
5. **Renderizador**: ParaLLEl-RDP é bit-exato (ótimo para o Legacy), mas limita texturas HD e interpolação; essas duas exigiriam um renderizador de alto nível (RT64) — decisão de arquitetura, não de ajuste.

## 7. Fontes e evidências

- `recomp/rsp/WDC_UCODE_SPEC.md` §2–§6 (ucode), §11 (RDP), §20–§21 (jogo, 60 fps, congelamento).
- `thirdparty/N64ModernRuntime/ultramodern/src/events.cpp` (thread de VI a 60 Hz, `get_target_framerate`).
- `thirdparty/parallel-rdp-standalone/parallel-rdp/video_interface.hpp`, `rdp_device.hpp` (opções de scanout e flags de upscaling).
- `recomp/project/src/wdc_window.cpp` (apresentação GDI), `wdc_rdp_vk.cpp` (thread do RDP, `scanout_sync`), `main.cpp:515` (8 MB de RDRAM para o RDP), `wdc_audio.cpp`, `wdc_input.cpp`.
- Medições: `recomp/explore/demo/*` (intro, Time Attack 60 vs 30 Hz), `recomp/explore/stall/*` (congelamento), dump de RAM `recomp/explore/demo/h2/ram20.bin` (tabela de modos de tela).
- Taxa do painel: `Win32_VideoController` (Intel, 2560×1600 @ 120 Hz).
- Conhecimento externo ao manual do N64 usado aqui: comportamento geral do RT64 e do ParaLLEl-RDP; **não** foi extraído do *Introduction to the N64* nem verificado nesta base além do que está citado acima.

## 8. Estado de implementação: M0 e M1 (2026-10-28)

### 8.1 Como usar

| Quero | Comando |
|---|---|
| Jogar em Enhanced | `build/wdc_recomp.exe --play --mode enhanced` |
| Jogar em Legacy (recompilação pura) | `build/wdc_recomp.exe --play` (padrão: `mode = "legacy"`) |
| Mudar o padrão | editar `<pasta do exe>/wdc_config/wdc_config.toml` (criado na 1ª execução, comentado) |

Precedência: **linha de comando > `wdc_config.toml` > padrão do modo**. Opções da linha de comando: `--mode legacy|enhanced`, `--fps60 on|off`, `--scale 1|2|4|8` (só Enhanced), `--vsync on|off`, `--filter nearest|bilinear|sharp`, `--present d3d11|gdi`. `WDC_RDP_SCALE=N` continua valendo como ferramenta de desenvolvimento (em qualquer modo).

```toml
mode = "legacy"            # "legacy" | "enhanced"
[enhanced]                 # só valem com mode = "enhanced"
fps60 = true               # corrida a 60 Hz (demo e replays continuam a 30 Hz)
internal_scale = 2         # 1, 2, 4 ou 8
[display]                  # valem nos dois modos (só mudam como a imagem chega à tela)
vsync = "on"               # "on" | "off" (off = apresenta ao ficar pronto; com VRR, sem rasgos)
filter = "sharp"           # "nearest" | "bilinear" | "sharp"
backend = "d3d11"          # "d3d11" (GPU, v-sync) | "gdi" (antigo)
```

No **Legacy** `fps60` e `internal_scale` são ignorados (com aviso se vierem da linha de comando): nenhum patch do Enhanced fica ativo. Cada modo usa o seu **Controller Pak**: `wdc_pak0.mpk` (Legacy) e `wdc_pak0_enhanced.mpk` (Enhanced). O título da janela mostra o modo.

### 8.2 M0 — um binário, dois modos

- `wdc_config.{h,cpp}`: leitura de um subconjunto de TOML + CLI + padrões por modo; `wdc_cfg()` é consultado pelo RDP (escala interna), pelo apresentador e por `main.cpp`.
- **Hooks condicionais**: o antigo build `--exp60` (C gerado à parte) foi removido. `gen_wdc_toml.py` agora emite, no C normal (não no `--trace`), cinco `ENH_HOOKS` que só agem com `g_wdc_enh_fps60`: (1) `func_800013DC` 0x800015BC: arma `g_wdc_skip_recv`, que faz `wdc_blk_osRecvMesg` dispensar a 2ª espera de VI; (2) 0x800015D4: força `$v0 == $t0` no `beq`; (3) `func_8002C500` 0x8002C584: divisor `n` e período nominal do `dt` por modo de reprodução; (4)(5) `func_80003DA4` (gravação de replay a 30 Hz). Com a chave em 0 o C executa exatamente o código original.
- **Medido** (Time Attack em tempo real, mesmo roteiro): Legacy ≈ 30 tarefas/s (59 por 2 s), Enhanced ≈ 60; volta de 20,93 s (Legacy) e 20,69 s (Enhanced), dentro da variação do roteiro (±1%). Build normal e trace: **0 avisos**.

### 8.3 M1 — apresentador em GPU (`wdc_present.{h,cpp}`)

- **D3D11 + DXGI flip model**, thread própria (prioridade alta), swapchain com objeto de espera de latência e `Present(1)` (v-sync) ou `Present(0)` + tearing permitido (VRR) com `vsync = "off"`. Escala por shader HLSL (compilado em tempo de execução): **nearest**, **bilinear** e **sharp-bilinear**, imagem 4:3 centralizada com barras (o campo de 640×240 vira 4:3). Tela cheia sem bordas (F11), minimizar/restaurar e redimensionar refazem o swapchain. Se o D3D11 falhar (ou `--present gdi`), cai no GDI (agora sem bloquear o RDP, spec §21.2).
- **A thread do RDP nunca espera o apresentador**: `wdc_present_frame` só copia o quadro e publica um ponteiro.
- **Ritmo** (v-sync ligado): o ultramodern gera retraços de VI a 60,000 Hz exatos, mas a thread do RDP entrega os quadros em rajadas. O apresentador reconstrói a linha do tempo regular do VI (`T_k = base + k/60 s + 17 ms`, base = menor `t_vi − k/60` visto, com ressincronização se o VI pular retraços) e **agenda por contagem de vblanks** (um quadro dura `round(período do VI / período do vblank)` vblanks: 2 a 120 Hz, 1 a 60 Hz) com histerese de ¾ de vblank; o período do vblank vem do modo de vídeo do monitor (Hz) refinado pela mediana dos últimos 16 intervalos (±10%). Latência do swapchain: 2 quadros por padrão (`WDC_PRESENT_LATENCY=1` reduz a latência, com mais vblanks perdidos).
- **Medido** (monitor 2560×1600 @ 120 Hz, jogo a 60 fps, janela, Intel): **93–99% dos quadros ficam exatamente 2 vblanks** (alternar 1/3 era ~14% com "o mais recente a cada vblank"), descartes < 1%, atraso VI→entrega do quadro < 10 ms em 97–99% dos quadros. Variação entre rodadas = carga da máquina. Estatísticas no log (`[present] …`) ao fechar.
- **Robustez** (script `scratchpad/stall_test.py`, com áudio ligado): 80 s com 8 ações (F11 ×4, minimizar ×2, restaurar ×2): 0 congelamentos; tela cheia 2560×1600 verificada por captura do back buffer (`WDC_PRESENT_SHOT=arquivo.bmp`, `WDC_PRESENT_SHOT_AT=s`).

### 8.4 Pendências e limites conhecidos

1. **Zero-cópia GPU→GPU**: o scanout ainda é lido de volta para a CPU (`scanout_sync`) e reenviado ao D3D11; funciona (60 fps a 2×), mas a 4×/8× o custo de memória cresce. Alternativa: exportar o scanout do ParaLLEl por memória externa (já há `export_scanout`) e compartilhar com o D3D11.
2. **Vários monitores/refresh não múltiplo de 60** (ex.: 144 Hz): o agendamento corrige por ±1 vblank (padrão 2-3); com VRR use `vsync = "off"`.
3. **Escala 4×/8× com o apresentador** ainda não medida; retrovisor/readback/menus 2D em escala interna > 1 seguem sem teste (§3.1).
4. **Instaladores e pasta de config**: a pasta padrão é a do executável (`wdc_config`); trocar de pasta de build deixa os saves para trás (use `--config` fixo ou copie o `.mpk`).
5. **Física a 60 Hz**: só o Time Attack ao vivo foi comparado; a sensação de direção precisa do teste de um humano (§6).

## 9. Controles e setup (2026-11-11)

### 9.1 Funções dos botões no jogo (base do mapeamento)
Manual oficial (via busca; o PDF só tem imagens, o texto exato da página de controles não foi lido) e medição no jogo: **A acelera**; **B freia forte** (87 → 34 → 0 kph em ~2,5 s) e, parado, dá ré; **L também freia**, mais suave (84 → 49 → 29 → 17 → 9 kph em ~6 s); **R sobe marcha** e **Z desce marcha** (câmbio manual; OPÇÕES › TRANSMISSION); **C-cima troca a visão** (confirmado: capô → externa → capô); **Start pausa**; nos menus os **botões C navegam as sub-telas**, A confirma, B volta, Z troca a cor do carro/espelha a pista. Estado do jogo usado pelo esquema moderno (RDRAM): `[0x8007DF94] == 1` corrida ao vivo, `[0x8007D794] != 0` pausa (medido: 0 correndo, 1 pausado), `[0x8007E530]` contador de ticks da simulação.

### 9.2 Esquemas do XInput (`[controls] scheme` em `wdc_config.toml`, `--controls classic|modern`; valem nos dois modos)
- **Clássico** (N64): A = A · B ou X = B · LT/RT = Z · LB = L · RB = R · Start · direcional · analógico direito = **botões C** (limiar 40%; antes 50%) · **Y = C-cima** (atalho). Todos os 14 botões do N64 + analógico ficam alcançáveis.
- **Moderno** (em corrida ao vivo, simulação andando): **RT = acelera** (A) · **LT = freia** (B) · **X/Quadrado = desce marcha** (Z) · **A/Cruz = sobe marcha** (R) · **Y/Triângulo = troca a visão** (C-cima) · LB/RB repetem descer/subir marcha. **Nos menus, na pausa e nos resultados = Clássico.** A detecção "em corrida" lê o modo de replay, o flag de pausa e se o contador de ticks mudou nos últimos 300 ms.
- Testes: `recomp/project/test/test_input.cpp` (+ `test_input_map.cpp`), `build_test_input.cmd` — 35 verificações, 0 falhas. No jogo: C-cima trocou a visão em Time Attack.
- Teclado inalterado (I J K L = botões C). Jogadores 2–4 usam o mesmo esquema nos seus controles XInput.

### 9.3 Setup (`wdc_setup.{h,cpp}`)
Janela Win32 desenhada com GDI+ (arte do anúncio do jogo embutida, `recomp/project/assets/setup_hero.jpg` → `wdc_setup_art.h` por `tools/recomp/gen_setup_art.py`): escolhe **Modo** (Legacy/Enhanced) e **Controles** (Clássico/Moderno), grava `mode` e `[controls] scheme` em `wdc_config.toml` (preservando os comentários) e abre o jogo. Aparece ao abrir o exe **sem argumentos** ou com `--setup`; atalhos 1–4, Enter, Esc; `--setup-shot arquivo.png` captura a janela (depuração). Fluxo testado por teclas (2, 4, Enter). Não foi feito no Claude Design: a ferramenta `DesignSync` só sincroniza arquivos com um projeto de design-system do claude.ai/design (skill `/design-sync`, iniciado pelo usuário), não gera telas.

## 10. Rodada "Leo Build" (2026-11-12): piscadas, IA, áudio, setup e pacote

### 10.1 Piscadas na tela (Enhanced) — causa e correção
- **Causa medida** (detector `WDC_FLICKER`, padrão A,X,A' em grade 80×60 de luminância; ver os quadros em `explore/arc/*/fl/`): a 60 Hz o jogo (sem a 2ª espera de VI, hook de `func_800013DC`) começa o quadro seguinte no buffer que o VI ainda exibe e a GPU pode estar no meio do quadro no scanout; aparecem quadros pela metade (só o céu azul/espelhado na contagem da corrida, só o fundo verde nos menus, carro só com as rodas). Sem filtro: 4–21 quadros intrusos por ~8 500 quadros (≈ 1 a cada 2–6 min); o Legacy também tem casos raros (1 em 8 209).
- **Tentativas descartadas**: portão por buffer (`WDC_GATE=1`, ainda no código, desligado: não reduziu) e "pular scanout com buffer sujo" (rastreio por SYNC_FULL/swap): o jogo chama `osViSwapBuffer` ANTES de o RDP terminar e desenha em várias passadas, então "sujo" marcava ~63% dos quadros → removido.
- **Correção (só Enhanced a 60 Hz, `g_deglitch` em `wdc_rdp_vk.cpp`)**: a thread do RDP segura cada quadro por UM quadro e o compara com o anterior (P) e o seguinte (F); se X difere muito de P e de F (> `WDC_DEGLITCH_T`, 8,0 de MAD na grade) e P ≈ F (< metade), X é uma piscada e P é reexibido. Custo: 1 quadro (16,7 ms) de latência; o apresentador ganha 1 quadro de folga (`wdc_present_set_extra_latency_us`/`g_latency_us`). `WDC_NO_DEGLITCH=1` desliga.
- **Resultado** (rota Arcade/Time Attack, mesma build, 8 500 quadros): sem filtro 21 intrusos; com filtro 0 (50 quadros substituídos, quase todos fades de logo). O detector passou a medir o fluxo JÁ filtrado (`deliver()`).

### 10.2 IA a 60 fps — verificada, nada a mudar
Rota com `--task-clock`, jogador acelerando (`explore/arc/ps2_leg` × `ps2_enh`, 70 dumps de RDRAM, velocidades de x/z em `0x800BA7F0+i*0x18A0+{0x44,0x48}` por segundo do relógio de corrida `+0xA44`): nos 13 primeiros segundos de corrida jogador e IA têm a mesma curva nos dois modos (jogador 8,2/20,6/29,9/37,2 u/s no Enhanced × 7,6/20,2/29,3/36,4 no Legacy; média da IA 8,4/21,0/30,5/37,9/43,0 × 8,5/21,1/30,3/37,4/43,3; razão jogador/IA 0,89–0,98 nos dois). Depois os carros colidem e as trajetórias divergem (caos), como esperado. O relógio de corrida anda 1,0 s por s de parede nos dois modos (ticks 30/s × 60/s). Conclusão: física e IA usam o mesmo `dt` nominal; a sensação de "muito mais rápido que os oponentes" não vem de um erro de passo de tempo.

### 10.3 Áudio — estalos/"explosões"
- **Estado da evidência**: os estalos sustentados (80–250 saltos/s) só apareceram em corridas de Time Attack (`trackID 5`, `raceType 7`) de builds anteriores, rodando sob carga (detector de piscadas + 3–4 processos); em 10 corridas Arcade da build atual (com e sem atraso, com saída WASAPI real) houve 0–5 saltos > 6 000 em 110 s. Não consegui reproduzir o cenário sustentado com a build final (rota por tempo de parede cai em Arcade), então o atraso que tentei como proteção piorou o som (ver abaixo) e foi desfeito. 8× travou o jogo (ver 10.4), então não serviu de teste de carga.
- Medido (WAV da AI, `--wav`): no Legacy 0 saltos de amplitude > 12 000 em 120 s de corrida; no Enhanced, rajadas de estalos de 4–5 amostras (a cada ~130 amostras, picos de 13 000–32 768) em builds sob carga (80–250 saltos/s durante toda a corrida) e, na build atual, em rajadas curtas ocasionais (56 saltos em 121 s). Não tem nada a ver com clipping (0 amostras em ±32767 no Legacy).
- **Causa**: a AI emulada lê a RDRAM no instante em que o quadro "toca" e o jogo escreve o anel de 16 KB pouco à frente; se a tarefa de áudio do RSP atrasa (CPU ocupada, mais tarefas gráficas por segundo a 60 Hz), a AI toca dados velhos do anel (da volta anterior) = estalo. A saída real (WASAPI) lê no instante exato, sem a folga que o modelo nulo dos testes tinha.
- **Correção tentada e DESFEITA (2026-11-12)**: ler a RDRAM com atraso (`g_lag_frames`, 60 → 40 ms) para dar folga à tarefa de áudio. Na máquina do usuário o som passou a **picotar o tempo todo**: o anel que o jogo reescreve não garante que o dado de há 40 ms ainda esteja lá (o avanço com que o jogo escreve não foi medido; assumi ~120 ms de folga sem evidência). A leitura voltou ao instante nominal (código original); com ela, 43 s de Enhanced com saída real tiveram 0 saltos > 6 000, 0 faltas no dispositivo e nenhuma descontinuidade nas fronteiras de DMA. A causa dos estalos esporádicos continua **não resolvida** (hipótese: tarefa de áudio atrasada → dado velho do anel).

### 10.4 Setup: controles personalizados e imagem
- **Personalizar controles** (página do setup, `[P]`): 6 ações (`accelerate`, `brake`, `soft_brake`, `shift_up`, `shift_down`, `camera` = A, B, L, R, Z, C-cima do N64) por esquema (`[controls.classic]`, `[controls.modern]`; moderno só em corrida, menus usam o clássico). Fontes: A B X Y LB RB LT RT L3 R3 BACK, várias com `+` ("B+X"), `none`, `default`. Clique na caixa do meio e aperte o botão no controle (captura por XInput; espera soltar tudo antes) ou use ‹ › (sem controle); Del = sem botão; assigna tira a fonte das outras ações do esquema (sem disparo duplo). API em `wdc_input.h` (`wdc_input_binding_*`, `wdc_input_configure`); 35 + 14 verificações em `test/test_input_map.cpp`.
- **Imagem** (página do setup, `[I]`): `enhanced.internal_scale` = `"auto"` (4× em GPU dedicada, 2× em integrada; `wdc_cfg_resolve_auto_scale`), 1, 2, 4 (**8× foi retirado**: a 2560×1920 o jogo praticamente para, ~25 tarefas gráficas em 90 s numa RTX 4050; a configuração limita 8 a 4); `display.aspect` = `"4:3"` | `"stretch"` (preenche a janela); `display.filter` = `nearest|bilinear|sharp|bicubic` (novo: Catmull-Rom em 9 amostras, `sample_catmull_rom` no HLSL); `display.vsync`; `display.fullscreen` (abre em tela cheia). Resolução interna só vale no Enhanced (a página avisa e escurece as opções no Legacy).

### 10.5 Pacote "Leo Build" (`tools/release/`)
- `build_release.cmd`: `-DWDC_RELEASE=ON` → CRT estática (/MT, sem Visual C++ Redistributable), aplicativo de janela (`/SUBSYSTEM:WINDOWS`, sem console; log em `wdc_config/wdc_log.txt`), sem caminho de ROM de desenvolvimento e sem o RSP de referência. `make_package.py` monta `dist/WorldDriverChampionship-LeoBuild-0.1.0-canary-win64.zip` (+ sha256) com o exe, `LEIA-ME.txt` e `licenses/` — **sem a ROM**.
- ROM: `wdc_rom.{h,cpp}` + `wdc_locate_rom` (main.cpp): `--rom` > cópia guardada `wdc_config/wdc.z64` > `wdc.z64`/`.z64/.v64/.n64` ao lado do exe (ou em `rom/`) > seletor de arquivo (repete até acertar; mensagens em PT-BR por tipo de erro). O librecomp já aceita as três ordens de byte e valida o hash.
- Config/saves em `<pasta do exe>/wdc_config` (portátil); se não gravável, `%LOCALAPPDATA%/WorldDriverChampionship`.
- Remendo no runtime (`thirdparty/N64ModernRuntime/librecomp/src/recomp.cpp`): os 4 GiB de endereços da RDRAM agora são **reservados** e só `mem_size` (512 MB) é **comitado** (antes `MEM_COMMIT` nos 4 GiB, que conta contra o limite de commit do Windows e derrubava o jogo em PCs com pouca memória).
- Setup: botão "atalho na área de trabalho" (IShellLink), versão "Leo Build 0.1.0 • canary" (`wdc_version.h`), ícone do carro (`assets/wdc.ico` → `src/wdc.rc`), `--version`.
- **Licença**: o runtime (N64ModernRuntime) é GPL-3.0 → quem recebe o binário tem direito ao código-fonte do projeto. O repositório publica o fonte do projeto SEM a ROM e SEM o C/ucode recompilados nem `config/asm` (derivados do jogo); isso precisa ser dito na página da versão.

### 10.6 Saves em pasta fixa (regra do usuário, 2026-11-12: "manter meus saves entre as builds")
- `wdc_saves.{h,cpp}`: o Controller Pak (`wdc_pak0.mpk` Legacy, `wdc_pak0_enhanced.mpk` Enhanced) fica em `%LOCALAPPDATA%/WorldDriverChampionship/saves`, igual para qualquer build ou pasta do exe. O save antigo ao lado do exe (`wdc_config/`) é **copiado** para lá se o destino ainda não existe (nunca sobrescreve, nunca apaga); cada abertura grava um backup datado em `saves/backups/` (não repete se igual ao último; mantém 10 por save).
- `wdc_pak_init` guarda um arquivo de tamanho errado como `<pak>.invalido` antes de criar um pak de fábrica (antes o sobrescrevia).
- Com `--config <pasta>` (testes) os saves ficam nessa pasta, para os testes não tocarem nos saves reais. Teste: `recomp/project/test/test_saves.cpp` (`build_test_saves.cmd`) + execução de ponta a ponta com `LOCALAPPDATA` isolado (cópia, original intacto, backup, pak carregado do novo lugar).
- Cópia de segurança dos saves de 2026-11-12: `dist/saves_backup_2026-11-12/` (os dois paks de `build/` e o de `build-exp/`).

### 10.7 Relato do usuário sobre a 0.1.1 a 4× (2026-11-12) e o que mudou
- **Log da sessão do usuário** (release 0.1.1, `wdc_log.txt`, 8,8 min): Enhanced 4×, V-Sync **desligado**, bicúbico, tela esticada em 2560×1600; 31 620 quadros a 60 fps estáveis (mediana 60,5 tarefas/s); 0 faltas de áudio no dispositivo; **uma queda de ~4 s aos 507 s** (13,8 e 20,5 tarefas/s com os retraços normais) e, nela, **51 vezes `ERROR --- Insufficient Free Buffers`** — a rotina de DMA de amostras do áudio do jogo (`func_80042CA0`, pool de 32 buffers liberados por idade em `func_80042FA8`) ficou sem buffer livre e reusa o buffer 0 (amostras trocadas = estouro/picote). Essa mensagem só aparece no Enhanced (nunca em logs do Legacy). Respostas do usuário: rasgos horizontais, quadro piscando, bordas/texturas estranhas, travadinhas; áudio estourado e picotando.
- **No laboratório** (4× sozinho, rota Arcade): 60 tarefas/s estáveis, 1,2 GB de VRAM, 1 quadro intruso em 9 005, 0 casos de dois quadros intrusos. Não reproduzi a queda de 4 s nem os outros defeitos; duas instâncias de 4× ao mesmo tempo travam (disputa de GPU).
- **Rasgos**: com V-Sync desligado e painel sem VRR a imagem rasga por definição (o setup avisa); recomendado deixar LIGADO.
- **Mudanças** (nenhuma verificada na máquina do usuário): bicúbico agora é **Mitchell-Netravali** (B=C=1/3), sem o halo do Catmull-Rom; **folga adaptativa do apresentador** (`g_lag_peak`: acompanha o atraso real de entrega do RDP; a 4× com V-Sync ligado a cadência foi de 65% para 95% dos quadros com 2 vblanks); `wdc_process_tuning()`: `timeBeginPeriod(1)` e desliga EcoQoS/"ignorar timer" (hipótese: o Windows reduz a energia do jogo em segundo plano → queda de quadros e áudio cortando); áudio: buffer WASAPI de 100 ms preenchido até 50 ms, thread com MMCSS "Pro Audio", **volume mestre 0,75 + limitador suave** só na saída do dispositivo (`WDC_AUDIO_GAIN`, 0,1–1,0; o WAV e o jogo seguem com o sinal original).
- **Diagnóstico novo no log**: `[hitch …]` (quadro com mais de 120 ms: intervalo, espera da GPU, fila do RDP e se a janela estava em primeiro plano), `[audio …] AI sem dados`, `[audio …] dispositivo sem quadros` e contador final de faltas no dispositivo. Detector de dois quadros intrusos (`flicker2`) no `WDC_FLICKER`.
- **Aberto**: causa da queda de 4 s e do esgotamento do pool de buffers de áudio (hipótese: áudio atrasado → pedido grande de DMA de uma vez); o filtro de piscadas só cobre quadros isolados.

### 10.8 Controles que não são XInput (2026-11-12): "meu controle Bluetooth não funciona"
- Causa: só havia XInput; PlayStation, Switch Pro, 8BitDo e genéricos aparecem como dispositivos DirectInput. Novo `wdc_joy.{h,cpp}`: backend **DirectInput 8** (`dinput8`/`dxguid`) que lista controles de jogo, **ignora os que já são XInput** (caminho de dispositivo raw com `IG_`, para não duplicar) e entrega o estado no mesmo formato do XInput (`WdcPadRaw`): o mapeamento clássico/moderno e os botões personalizados valem sem mudar nada. Procura controles a cada 2 s sem nenhum e a cada 5 s com algum (conectar/desconectar com o jogo aberto).
- Perfis por VID/PID (botões numerados do DirectInput → botões lógicos A,B,X,Y,LB,RB,LT,RT,BACK,START,L3,R3): Sony `054C` (A=1,B=2,X=0,Y=3,LB=4,RB=5,LT=6+eixo 4,RT=7+eixo 5,BACK=8,START=9,L3=10,R3=11), Nintendo `057E`, 8BitDo `2DC8` modo "Android" (A:0,B:1,X:3,Y:4,LB:6,RB:7,LT:8,RT:9,BACK:10,START:11,L3:13,R3:14), genérico estilo Logitech (A:0,B:1,X:2,Y:3,LB:4,RB:5,BACK:6,START:7,L3:8,R3:9). **Verificado em hardware só o 8BitDo do usuário** (Bluetooth); o perfil do DualSense segue a numeração documentada e **não foi testado**. Direcional = hat (POV); analógicos por eixos X/Y e Z/Rz (ou RX/RY); gatilhos por botão ou eixo (`ax<N><m|M|p|n>` no texto do mapa).
- **Assistente** (setup → PERSONALIZAR → "MAPEAR OUTRO CONTROLE…" ou `M`): pede os 12 botões em ordem, esperando soltar tudo entre um e outro (e guardando o repouso dos eixos), aceita eixo nos gatilhos, tem PULAR/Esc, e grava na hora em `[gamepad]` de `wdc_config.toml` (`dev_<vid>_<pid> = "A:0,B:1,…"`). O mapa vale por modelo de controle e substitui o perfil. Páginas verificadas por captura de tela; o fluxo com botões reais foi exercitado com o 8BitDo.
- `WorldDriverChampionship.exe --joytest [segundos]` (`wdc_joy_test`): lista os dispositivos e imprime cada mudança de botão/eixo/hat — é como se descobre a numeração de um controle novo.
- Testes: `test_input_map.cpp` cobre perfis, aprendizado (minúsculas, espaços, nome/índice inválidos), chave `dev_vvvv_pppp` e a substituição pelo config (todos passam).

### 10.9 "Motion blur" da abertura (2026-11-12) — o que é e o que mudou
- **Pedido**: "por que tem esse efeito de motion blur na intro? Retira isso" (resposta do usuário: borrão nos cenários em movimento).
- **O que NÃO é**: (a) feedback de quadro — o despejo do RDP (`WDC_RDPDUMP`) e o contador `WDC_FBTEX` mostram 0 desenhos que leiam um framebuffer como textura; (b) mistura temporal do VI (`blend_previous_frame` desligado; o desentrelaçamento do ParaLLEl é "bob" por leitura direta, sem mistura); (c) filtros do VI em Enhanced ≥ 2×: com a resolução ampliada, desligar o filtro de dither do VI não muda nenhum pixel (verificado por diferença de imagem), e AA/escala do VI mexem em ~21% dos pixels (bordas). Em Legacy 1× o filtro de dither do VI borra um pouco (nitidez média 1,69 → 2,19), mas Legacy é o modo "puro" e não foi mudado.
- **O que é**: a abertura (e boa parte da corrida) usa texturas minúsculas (blocos de 2 KB: 64×32 CI8 / 32×32 RGBA16, uma textura diferente por pedaço de parede/pista, ~84 cargas por quadro) que ficam **muito ampliadas** na tela, e o RDP do N64 filtra com o **filtro de 3 pontos** (`sample_texture` em `parallel-rdp/shaders/texture.h`: no lugar dos 4 texels usa 3, conforme `frac.x+frac.y`). Esse filtro espalha cada texel na **diagonal** e, com o cenário em movimento, vira riscos/"motion blur" (parede tipo "janela borrada", riscos radiais na pista, vistos também na corrida). Com amostragem **por ponto** a mesma cena sai em pixels nítidos, sem riscos (comparação: `recomp/explore/arc/pt_cmp.png`, `rp_cmp.png`).
- **Mudança**: Enhanced ganhou `[enhanced] sharp_textures = true` (padrão; `false` = filtro original) e o grupo "TEXTURAS" no setup. Implementação: `RDP::g_wdc_point_sampling` (remendo `tools/release/patches/parallel-rdp-point-sampling.patch`) faz `op_set_other_modes` limpar `RASTERIZATION_SAMPLE_MODE_BIT` (o bit de "amostra bilinear"), o que leva o shader ao caminho de amostragem pontual; ligado por `wdc_rdp_vk_init` conforme `wdc_cfg().sharp_textures` (só Enhanced) ou `WDC_POINT_TEX=1` (teste, qualquer modo). **Legacy não muda** (quadro de controle idêntico byte a byte ao de antes).
- Verificado em captura: parede e pista da abertura sem riscos; corrida (rota r12) com parede/estrada nítidas; menus, pak, texto e logotipos corretos; nada sumiu. **Não verificado**: todas as pistas/carros (alguma textura que dependa do filtro bilinear, como bordas translúcidas, pode ficar mais "quadriculada"); gosto — as texturas viram pixels; quem preferir o macio original escolhe "ORIGINAL N64".
- Ferramentas de diagnóstico mantidas: `WDC_VI_OFF=aa,scale,serrate,dither,divot,gamma`, `WDC_VI_WEAVE`, `WDC_RDPDUMP=<pasta>@<s>` (agora com todas as palavras dos triângulos).

### 10.10 Letterbox da Boss -> tela cheia: tentativa DESCARTADA (2026-11-12 a 15; o código foi removido)
- **Pedido** do usuário: tirar o letterbox dos previews de pista e das telas de vitória, só no Enhanced. Implementei, o usuário testou a dev build 0.1.3 em 2026-11-15 e mandou **desfazer ("não ficou legal")**: a mudança saiu do código, do setup e do gerador de C (o C regerado é idêntico, byte a byte, ao de antes). Esta seção fica só como registro do que se descobriu.
- **Relato do usuário na 0.1.2** (4×, 7 min a 60 fps, controle 8BitDo reconhecido, 0 estouros de áudio; os "4302 quadros" de silêncio são uma única pausa do próprio jogo em 5,1 s): *texture pop-in* nas corridas de Hawaii A e C (não em Kyoto A). **Hipótese não verificada**: efeito das texturas por ponto (o jogo troca texturas de baixa por alta resolução com a distância; o filtro de 3 pontos suavizava a troca). Teste: Imagem → Texturas → Original N64.
- **Como o letterbox é feito** (útil para quem tentar de novo): o jogo tem 9 "modos de tela" (descritores de 0x74 bytes em `0x800AAE38`, montados por `func_800006E0`; `func_800010A0(descritor)` troca o atual, apontado por `D_8007D75C`; `func_8000110C(índice)` é o invólucro): `+0` largura, `+4` altura do framebuffer, `+0x14` float de proporção do pixel, `+0x18` float de proporção da tela (tabela de floats na ROM em `0x9ACBC`: seis modos 4:3 e **três 8:3**), e o `OSViMode` embutido em `+0x24`. O **modo 6 é o letterbox** (framebuffer **320×240**, tela 8:3, pixel 2:1, VI entrelaçado numa janela vertical de meia altura: `V_START` 0x009A0188 em vez de 0x002301FD); 7 e 8 (640 de largura) são os irmãos de 4 e 5 e nunca foram pedidos nas rotas r2 e r11. Não é uma máscara desenhada: a cena é renderizada com proporção 8:3 e o VI a mostra achatada. Serve ao preview de pista, às telas de vitória/resultado em 3D e à abertura.
- **O que foi feito**: ganchos condicionais (`ENH_HOOKS`) em `func_800010A0` (trocava o descritor do modo 6 pelo do **modo 0**: mesmo framebuffer 320×240, tela 4:3, VI em quadro cheio) e em `func_80011D54` (recalculava o `fovy` de `guPerspectiveF` com `tan(fovy'/2) = 2·tan(fovy/2)` para manter o campo de visão horizontal). Em capturas, abertura, preview ("ROME A", "HAWAII A") e todas as telas de resultado da rota r11 ocupavam o quadro 4:3 inteiro, sem queda. Efeito colateral: aparece mais cena do que a Boss projetou (abertura: cunha preta no topo, faixa lisa no chão).
- **Lição de memória**: trocar o 6 pelo modo 3 (320×480, o equivalente "normal") derrubava o jogo ao fim da corrida (violação de leitura logo após a troca; framebuffer de 2× os bytes invade memória usada nas telas de resultado, RDRAM de 4 MB). Só o modo 0 (mesmo framebuffer) é seguro.
- Ferramentas mantidas: `WDC_VI_LOG=1` agora registra só mudanças de modo do VI (ignora o `ORIGIN`), `WDC_RDPDUMP=<pasta>@<s>` grava todas as palavras dos triângulos.

### 10.13 Abertura e replay "pipocando" no Enhanced: ordem de exibição (2026-11-15, pós-0.1.5)

- **Relato do usuário**: a abertura e o modo replay "sempre têm algum bug gráfico", "o gráfico fica pipocando", "o motion blur fica estranho".
- **Causa (medida)**: as telas de 30 Hz (abertura, replay, demo) no Enhanced. Sem a 2ª espera de VI (`func_800013DC`) o jogo desenha o próximo quadro no buffer que o VI ainda exibe. Rastreando cada tick de VI (`WDC_FLICKER_TRACE=t0,t1`: origin, color images desenhadas, comandos): cada buffer fica 2 ticks na tela e o quadro novo (~900 comandos) é desenhado nele no **2º tick**; o 1º tick de cada par mostra o quadro de **dois quadros atrás**. A imagem exibida anda `F0 F2 F1 F3 F2 F4...`: vai e volta no tempo a cada tick (não é textura nem filtro: o mesmo acontecia com `sharp_textures` desligado). Na abertura (46 s): 68 trocas de buffer a cada tick contra 1 no Legacy; saltos de luminância entre quadros 62 contra 13; 6 quadros "intrusos" que o filtro temporal precisou apagar. No replay embutido (R5): 177 saltos contra 93 no Legacy, 16 intrusos. Corrida a 60 Hz: não afetada (o padrão só existe quando o jogo desenha a cada 2 ticks).
- **Correção** (`wdc_rdp_vk.cpp`, só Enhanced; `WDC_NO_ORDER=1` desliga): cada quadro COMPLETO (SYNC_FULL) recebe um número de série, gravado nos color images que ele usou. Se o VI pede um buffer de série **menor** que a do último quadro exibido, o scanout reapresenta o último quadro (mesmo caminho do filtro temporal) em vez de voltar no tempo; no máximo 1 vez seguida (nunca congela a tela por engano) e recomeça a cada mudança de modo de vídeo. Efeito nas telas de 30 Hz: cada quadro novo aparece uma vez e fica 2 ticks, em ordem (~1 tick a mais de latência, só nelas).
- **Resultado**: abertura 62 → 16 saltos (Legacy 13), 0 quadros intrusos; replay 177 → 87 (Legacy 93), 16 → 1 intrusos; campeonato completo (menus + corrida a 60 Hz): 340 repetições em 8.623 quadros, todas nas telas de 30 Hz; janela em 4x/V-Sync: 99% dos quadros ficam exatamente 2 vblanks. Legacy: quadro de controle idêntico byte a byte. **Não verificado**: a olho em movimento na máquina do usuário (medi a ordem dos quadros e os saltos, não olhei o vídeo); o filtro de textura por ponto continua sendo uma escolha à parte (serrilhado em textura minúscula ampliada).
- **Ferramentas novas**: `WDC_FLICKER_LOG=1` (lista os saltos), `WDC_FLICKER_TRACE=t0,t1` (uma linha por tick de VI), `WDC_TRILOG=1` (triângulos/retângulos/texturas por quadro), histograma de ticks seguidos no mesmo buffer no resumo do detector.
- **Em aberto (pedido do usuário, não resolvido)**: "gráficos à distância": o carro vermelho da Reeds parece "algo de longe" e fica mais detalhado de perto. Medi: a maioria dos desenhos usa mipmap do RDP (`SET_OTHER_MODES` com LOD de textura ligado: 611 de 735 no quadro examinado), o ParaLLEl já seleciona mipmap por pixel da resolução interna (a 4x escolhe níveis mais nítidos que o N64), o ucode gráfico é de fluxo (vertex_xform/triangles em blocos encadeados) e a escolha de nível de detalhe do MODELO é código da CPU que não localizei (sem símbolos; a tabela de floats parecida com distâncias em 0x90CFC é de desempenho dos carros, não de LOD). Faltam: um repro com o carro vermelho (pista, modo, distância) e achar a rotina de seleção de modelo.

### 10.12 Desempenho em PCs fracos (0.1.5, 2026-11-15)

- **Pedido**: o jogo engasgava, mesmo em Legacy, no PC do pai do usuário ("um pouco mais fraco"). Não havia log desse PC (a 0.1.4 já grava logs; o usuário vai mandar o da 0.1.5). O que se fez foi medir onde o tempo vai e fechar o que podia ser causa, sem alterar o que o Legacy desenha num PC que acompanha.
- **Medições (este PC: Core 5 210H, RTX 4050 + Intel Graphics; janela, Legacy, rota até os menus)**: o custo por quadro do RDP é < 8 ms em 99,5% dos quadros e a CPU do processo fica em 14-20% de um núcleo (as rodadas com 40-60% coincidiram com carga externa na máquina). O custo previsível que sobra é o **scanout do VI a 60 Hz** (2,3-3,7 ms cada: passe de VI na GPU, espera, leitura de ~1 MB, cópia e envio ao apresentador): em Legacy o jogo desenha 30 quadros/s, então metade dos scanouts é a mesma imagem. Numa GPU 5-8x mais lenta isso sozinho passaria de 1 s por segundo na thread do RDP.
- **(1) Repetição de scanout** (`wdc_rdp_vk.cpp`, `process_vi`): se os registradores de VI são os mesmos, nenhum comando de desenho foi para o buffer exibido desde o último scanout real (rastreio por `SET_COLOR_IMAGE`) e o hash dos bytes do framebuffer na RDRAM não mudou, reapresenta o quadro anterior (`wdc_present_repeat`: sem copiar pixels nem reenviar a textura). Medido: **2.329 de 4.657 scanouts evitados (50%)**. Limite de **1 repetição seguida** (o par de ticks de cada quadro de 30 Hz). `WDC_NO_SCAN_REPEAT=1` desliga; `WDC_SCAN_VERIFY=1` refaz o scanout mesmo assim e compara. **Verificação**: de 2.328 candidatos, 2.327 diferem só por 1 nível em um canal (o ruído de dither do VI muda a cada scanout; a repetição mostra o mesmo ruído duas vezes) e **1 difere de verdade** (efeito de transição branco: mesma RDRAM e nenhum comando entre os dois scanouts; causa **não explicada**). Pior caso: um tick (16,7 ms) mostra a imagem do scanout anterior, em ~0,04% das repetições. Não vale no Enhanced (o filtro de quadro intruso segura quadros).
- **(2) Governador de quadros** (`display.performance = "auto"`, padrão; `--performance off|auto`; só **Legacy**): mede o custo de desenhar um quadro (do 1o lote até a GPU terminar, contando só o que pular quadros alivia: se a GPU acompanhou a produção dos comandos, o tempo é do RSP) mais o custo do scanout, e **só entra em ação se o jogo já está atrasado** (intervalo médio entre quadros > 10% acima de 33,3 ms) **e** o modelo diz que não cabe (orçamento de 78% do período). Então PULA o desenho de uma fração dos quadros: os comandos de estado e de carga de textura passam, os de desenho (`TRI*`, `TEXRECT*`, `FILL_RECT`) não; o `SYNC_FULL` é atendido na hora; o VI de um buffer cujo quadro foi pulado reapresenta o último quadro desenhado (casamento exato do origin do VI com o `SET_COLOR_IMAGE`). Mínimo de 34% dos quadros desenhados; só começa depois de ~5 s. **Teste com GPU fraca simulada** (`WDC_FAKE_FRAME_MS=30 WDC_FAKE_SCAN_MS=6`: 30 ms por quadro + 6 ms por scanout): sem governador o jogo rodou a **19,7 tarefas/s** (66% da velocidade real); com governador **28,4** (95%), desenhando ~60% dos quadros. Com custo intermediário (12 + 3 ms) e sem custo simulado o governador **não entra** (29,6 e 29,7 tarefas/s). Um defeito da primeira versão (buffer "velho" casado por faixa larga, congelando a tela em modos de vídeo largos) foi achado no teste de Enhanced e corrigido. **Não verificado**: o aspecto visual dos quadros pulados em jogo real (só se contou tarefas, não se olhou); efeitos que leem o framebuffer ficam um quadro mais velhos enquanto o governador age. No Enhanced o governador foi testado e **desligado**: com 4x o scanout custa 8-14 ms mas o jogo acompanha, e o governador pulava quadros sem ganho de velocidade.
- **(3) Compilação de pipelines sem travar** (`vulkan/command_buffer.cpp`, `vulkan/device.cpp`; patch `parallel-rdp-async-pipelines.patch`): na primeira execução com a GPU Intel desta máquina o log do Granite mostrou **45 "Stalled compile"** (39 em modo `fail-on-compile-required`, 150-450 ms cada, ~10 s no total) na thread de comandos: o driver Intel do Windows **não honra** `VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT` e compila na hora, travando o jogo a cada pipeline novo (cache frio). Correção: (a) para Intel (`vendorID 0x8086`) a tentativa "sem bloquear" é desligada (`broken_pipeline_cache_control`, o mesmo mecanismo que o Granite já usa para NVIDIA antiga e Qualcomm); (b) para qualquer driver, se uma tentativa demorar > 8 ms o programa conclui que o driver não honra a flag e desliga as tentativas dali em diante (a compilação fica só com a thread de fundo e o pipeline genérico de reserva cobre a espera). `WDC_STOCK_PIPE=1` volta ao comportamento original. **Não consegui reproduzir o cache frio do driver Intel depois da correção** (o driver guarda o próprio cache fora do alcance do programa, e apagá-lo mexeria em dados de outros programas): o efeito está justificado pelo mecanismo e pelo log das 45 compilações bloqueantes, e verificado só quanto a não haver regressão.
- **(4) Prioridades** (`WDC_NO_PRIO=1` desliga tudo): processo acima do normal; threads do RSP, de gráficos, do RDP, do jogo e de comandos do ParaLLEl acima do normal; a de VI no máximo (o runtime pede `Critical`, mas a chamada estava comentada no upstream: patch `N64ModernRuntime-thread-priority.patch`). Teste com 2 processadores lógicos + 2 processos queimando CPU: **sem ganho claro de velocidade média** (12 tarefas/s com prioridades; 24 e 13 sem, rodadas muito diferentes), mas o pior engasgo da apresentação caiu de **5,8 e 6,9 s para 0,67 e 0,93 s**. Fica pelo ganho de pior caso (programas em segundo plano), sem prova de ganho de velocidade. A tentativa de baixar a prioridade da thread de compilação de pipelines foi retirada (risco de ela nunca rodar sob carga, sem ganho medido).
- **Diagnóstico novo no log**: `[hb]` a cada 2 s com `cpu=NN%`; resumo no encerramento (custo por quadro do RDP em faixas de ms, tempo do scanout, repetições, governador, `[cpu]` por thread); linhas `[gov ...]` quando o governador muda de nível. Para analisar um PC que engasga: "intervalo entre quadros", "custo por quadro do RDP", "scanouts" e as linhas `Stalled compile` dizem se é GPU, CPU ou compilação de shader.
- **Legacy**: quadro de controle (`--task-clock --shot-at 21`) **idêntico byte a byte** ao da 0.1.4 (o governador e a repetição só agem com a janela aberta; a compilação assíncrona não muda o que é desenhado, só quando).

### 10.11 Áudio, música cortada, travadas raras, setup e logs (2026-11-15; versão 0.1.4)
- **Pedidos do usuário**: (1) chiado e "sons fora de lugar" (ranhura) nas corridas, quando todos os canais de áudio tocam juntos — "trocar codecs por algo mais moderno ou qualquer abordagem que deixe o som mais limpo"; (2) travadas raras; (3) o setup ficou com letras e resolução estranhas em outro PC; (4) um arquivo de log para analisar o que acontece em outros PCs; (5) as músicas de vitória, de rank e outras não tocam até o fim.
- **Não há codec para trocar**: o jogo mistura o áudio **na CPU** em PCM de 22 kHz num anel de 16 KB (§ spec 14); o que "troca de codec" aqui é a conversão 22 kHz → taxa do dispositivo (WASAPI, 48 kHz float). Era interpolação linear (deixa "imagens" espectrais e come agudos: chiado/aspereza em mixagens cheias). Agora é um **interpolador polifásico de sinc com janela de Kaiser** (32 pontos, 1024 fases, corte em 0,46 da taxa do jogo, β = 7,5; histórico dos quadros já tocados porque o jogo reescreve o anel logo atrás da posição de reprodução). `WDC_AUDIO_LINEAR=1` volta à linear para comparar. Não verificado de ouvido (sem alto-falante no laboratório); verificado só que roda sem estalos novos e sem falta no dispositivo.
- **Música cortada — causa encontrada e confirmada**: o jogo mede o áudio em **quadros**, não em segundos. `func_80048508` (máquina de estados da música, chamada 1×/quadro por `func_8000C560`) troca a música quando contadores de 932/1021/991/71 quadros estouram; a 60 Hz (o Enhanced roda o laço do jogo a ~58 tarefas/s inclusive nos menus; Legacy 30/s) isso acontece na **metade do tempo**. Medido com a mesma rota (r11, nova chave `--vi-clock`: relógio por retraços de VI, para a mesma rota valer nos dois modos): a música de vitória (música 14, carregada aos 125,4 s) era trocada aos **142,1 s no Enhanced contra 158,8 s no Legacy** (16,8 s contra 33,4 s: exatamente 2×, nas duas ocorrências da rota). Com o conserto, o Enhanced troca aos 158,75 s (igual ao Legacy) e todas as demais trocas de música coincidem com as do Legacy.
- **Conserto (dois ganchos `ENH_HOOKS`, só Enhanced)**: `wdc_audio_gate(id)` (em `wdc_audio.cpp`) pula uma chamada em cada duas quando o jogo está a 60 Hz (intervalo de 1 retraço entre chamadas); se o quadro durou 2 retraços ou mais executa sempre. Aplicado a `func_80048508` (música) e a `func_80042FA8` (a "bomba" de áudio, chamada 1×/quadro por `func_80024FCC`). Resultado medido: 48–49% das chamadas puladas em menus; em corrida 33% (a cadência da corrida alterna quadros de 16,7 e 33 ms). `WDC_NO_AUDIO_GATE=1` desliga (comparação A/B).
- **Por que a bomba também**: `func_80042FA8` mistura em passos de **um tick do sequenciador** (`22000·5/(2·tempo)` quadros; tempo = `D_8009289C`, vindo da música) a partir da posição da AI, e libera os buffers de DMA de amostras que **não foram usados em 3 chamadas SEGUIDAS** (contador por buffer em `D_800D5930`, flag de uso em `D_800D5910`, pool de 32). A 60 Hz, chamadas sem nenhum tick completo (tick > 16,7 ms) liberavam buffers ainda em uso: o próximo DMA reaproveitava o buffer → amostras trocadas (som fora de lugar) e, com muitos canais, `ERROR --- Insufficient Free Buffers` (visto 51× numa travada de 4 s do usuário na 0.1.1; nunca no Legacy). **Medido em laboratório (captura da AI, `--wav`, mesma rota nos dois modos com `--vi-clock`; "saltos" = diferença > 6000 entre amostras vizinhas)**: rota de resultados r11 (343 s): Legacy **0** saltos (pico 8283); Enhanced **sem** o conserto **5188** saltos (1154 acima de 12000) e pico **32768** (saturação); Enhanced **com** o conserto **0** saltos (pico 8758). Rota do campeonato r2 (442 s): sem o conserto 2503 saltos (511 > 12000, pico 25673), com o conserto 1 (pico 9522). Ou seja: o áudio do Enhanced a 60 Hz estava corrompido (estalos, picos saturados, amostras trocadas) e a 30 Hz volta a ficar igual ao Legacy. Qual das duas rotinas (bomba ou música) responde por qual parte não foi separado; a explicação mecânica dos buffers liberados cedo é coerente com o código mas não foi isolada. Antes, corridas curtas de teste (40–80 s, sem menus) tiveram 0 estouros nos dois casos: o defeito aparece nas sequências longas com menus, música e várias vozes.
- **Diagnósticos de áudio novos** (aparecem no log, resumo no encerramento): `ANEL VELHO` (≥ 64 quadros não silenciosos idênticos aos lidos uma volta antes: o jogo não reescreveu o anel a tempo e a AI tocou dado velho = som de "ranhura"), `bomba de audio do jogo parada por X ms` (quadro do jogo atrasado > 120 ms), `a AI andou N quadros entre duas leituras` (acima de 2048 = o limite de mistura do jogo por chamada, 93 ms) e o resumo `diagnostico: ...`. Nos testes: 0 anéis velhos, 0 pausas da bomba.
- **Travadas raras**: nos logs do usuário (4 sessões) todas as travadas registradas (133–247 ms) eram telas de carregamento ("Initialising Car Structure"), não corrida. O registro agora é mais fino: limiar de 60 ms (antes 120), com CPU do RDP no quadro, espera da GPU, fila, estado do jogo (`0x923E4`), flag de corrida (`0x7DF94`) e janela em primeiro plano; histograma do intervalo entre quadros e contagem de quadros com > 20 ms de CPU só enfileirando (compilação de pipelines) no encerramento. Novo: **cache de pipelines do Vulkan em disco** (`%LOCALAPPDATA%/WorldDriverChampionship/cache/pipeline_cache_<fornecedor>_<dispositivo>.bin`, gravado aos ~100 s e no encerramento) para as próximas execuções não recompilarem shaders do RDP no meio da corrida. Não verificado que isso elimina as travadas (não as reproduzi).
- **Setup compatível com outros PCs**: (a) DPI **por monitor** (`SetProcessDpiAwarenessContext` PerMonitorV2, com alternativas) e `WM_DPICHANGED` (arrastar a janela para um monitor de outro DPI reescala e redimensiona); (b) a escala é reduzida para a janela caber na **área útil** do monitor com folga de 16 px (1366×768 a 125–150%, 1080p a 175–200%) e a janela é centralizada na área útil, não na tela; (c) fontes: "Impact" e "Segoe UI" com lista de alternativas (Arial Black, Segoe UI Black, Arial, Tahoma, Verdana, Microsoft Sans Serif, sans-serif genérica) com fator de tamanho, escolhidas pelas famílias instaladas (`InstalledFontCollection`), e o log diz qual foi usada; (d) a janela do jogo também encolhe para caber (960×720 pedido) e abre centralizada. Testes locais: `WDC_SETUP_DPI` e `WDC_SETUP_WORK` (ex.: 144 e 1366x728) simulam outro PC e `WDC_SETUP_HIDE_FONTS="Impact;Arial Black"` finge fontes ausentes; verificado por captura (1066×656 para 1366×728 a 150%; 1015×625 para 1280×680 a 100%). Não verificado em outro PC de verdade.
- **Log para outros PCs** (`wdc_logs.{h,cpp}`): na build de distribuição, a saída vai para `%LOCALAPPDATA%/WorldDriverChampionship/logs/wdc_log_AAAAMMDD-HHMMSS.txt` (os 10 mais recentes; sem buffer, então sobrevive a queda), com cabeçalho `[sys]` (versão, Windows, CPU, memória, placas de vídeo com driver DXGI, monitores com resolução/Hz/área útil/DPI, dispositivo de áudio padrão, pastas, administrador, idioma) e os argumentos. Botão **"ABRIR PASTA DE LOGS [L]"** na página principal do setup. O log antigo em `wdc_config/wdc_log.txt` foi abandonado.
