# Roteiro de testes — World Driver Championship (recomp)

Para você validar o jogo de ponta a ponta nos dois modos (Legacy e Enhanced) e nos dois esquemas de controle (Clássico e Moderno).
**Prioridade:** P1 = faça primeiro (≈ 40 min, cobre o que mais pode quebrar), P2 = cobertura completa, P3 = canto raro.
**Auto** = já medi/automatizei (não precisa repetir, só conferir se algo parecer errado). Itens sem "Auto" só você consegue julgar (sensação, olho, ouvido, campanha longa).

## 0. Preparação

| | |
|---|---|
| Abrir (com setup) | duplo clique em `recomp\project\build\wdc_recomp.exe` (sem argumentos abre o **Setup**) ou `wdc_recomp.exe --setup` |
| Abrir direto | `wdc_recomp.exe --play --mode enhanced` · `--mode legacy` · `--controls modern` · `--controls classic` |
| Pasta de config e saves | `recomp\project\build\wdc_config\` — `wdc_config.toml`, `wdc_pak0.mpk` (Legacy), `wdc_pak0_enhanced.mpk` (Enhanced) |
| Guardar o log | rode pelo terminal: `wdc_recomp.exe --play --mode enhanced > log.txt 2>&1` (o jogo imprime avisos, estatísticas e `[hb] ATENCAO` se parar) |
| Teclas | setas = analógico · WASD = direcional · X = A · C = B · Z = Z · Enter = Start · Q/E = L/R · I J K L = botões C · F11 = tela cheia · Esc = sair |

**Ao reportar um problema**, anote: modo (Legacy/Enhanced), esquema (Clássico/Moderno), tela/pista/carro, o que fez, o que esperava, o que aconteceu, e se o `log.txt` tem `ATENCAO` ou `[win]`/`[present]` estranho.

## 1. Setup (launcher)

| # | Pri | Teste | Esperado |
|---|---|---|---|
| 1.1 | P1 | Abrir o exe sem argumentos | Janela de setup com a arte do jogo à esquerda, cartões de Modo e Controles, botões JOGAR e Sair |
| 1.2 | P1 | Escolher Enhanced + Moderno e clicar JOGAR | O jogo abre; o título da janela diz "Enhanced"; o log mostra `modo=Enhanced … controles=modern` (**Auto**: fluxo por teclas 2/4/Enter) |
| 1.3 | P1 | Fechar e abrir o setup de novo | Já vem com a última escolha marcada (**Auto**) |
| 1.4 | P2 | Teclas 1–4, Enter, Esc | 1/2 = modo, 3/4 = controles, Enter = JOGAR, Esc = sair sem abrir o jogo |
| 1.5 | P2 | Botão Sair / fechar no X | Fecha sem abrir o jogo e **sem** alterar o arquivo de config |
| 1.6 | P2 | Telas com escala de 125%/150% do Windows | Setup nítido e sem cortes (estava a 150% nas minhas capturas) |
| 1.7 | P3 | `wdc_recomp.exe --play` (com argumentos) | Pula o setup e usa o `wdc_config.toml` |

## 2. Modos Legacy × Enhanced

| # | Pri | Teste | Esperado |
|---|---|---|---|
| 2.1 | P1 | Corrida em **Legacy** | Fluidez de 30 fps, resolução original (imagem mais "pixelada"), nenhuma melhoria |
| 2.2 | P1 | Mesma pista em **Enhanced** | 60 fps visivelmente mais fluidos, bordas mais nítidas (escala interna 2×) |
| 2.3 | P1 | Menus, intro, demo e replay em **Enhanced** | Idênticos ao Legacy (demo/replay rodam a 30 Hz de propósito: foram gravados assim) (**Auto**) |
| 2.4 | P1 | **Time Attack, Hawaii A**: faça uma volta "limpa" em cada modo e anote o tempo | Tempos próximos (diferença típica ≤ 1–2%). Se um modo for muito mais rápido/lento, **reporte** (indício de física diferente) |
| 2.5 | P1 | Salvar a campanha no Legacy e abrir o Enhanced (e vice-versa) | Cada modo tem o seu Pak: o save de um **não** aparece no outro (esperado). Recordes também são separados |
| 2.6 | P2 | Taxa de quadros: rode com o terminal e olhe `[hb]` | Enhanced ≈ 60 tarefas/s em corrida (≈ 120 por 2 s); Legacy ≈ 30/s (**Auto**) |
| 2.7 | P2 | `--fps60 off` no Enhanced | Volta a 30 fps mantendo o resto das melhorias |
| 2.8 | P3 | `--scale 4` e `--scale 8` | Mais nítido; observe queda de desempenho (8× é pesado) e artefatos |

## 3. Controles (controle XInput)

> Câmbio manual: em **OPÇÕES › GAME SETTINGS › TRANSMISSION** escolha **MANUAL** para testar as marchas. Visão do carro: **CAR VIEW**.
> Funções dos botões no jogo (manual oficial + medição): **A acelera, B freia forte e depois dá ré, L também freia (mais suave), R sobe marcha, Z desce marcha (câmbio manual), C-cima troca a visão, Start pausa**; nos menus os **botões C navegam as sub-telas**, **A** confirma, **B** volta; na seleção de carro o **Z** troca a cor; na seleção de pista o **Z** espelha a pista.

### 3.1 Esquema Clássico (como no Nintendo 64)

| # | Pri | Botão do XInput | Esperado |
|---|---|---|---|
| 3.1.1 | P1 | Analógico esquerdo | Volante proporcional (zona morta de 20%) |
| 3.1.2 | P1 | A | Acelera |
| 3.1.3 | P1 | B (ou X) | Freia forte; parado, dá ré |
| 3.1.4 | P1 | LB | L: freia (mais suave) |
| 3.1.5 | P1 | RB / LT ou RT | R sobe marcha · Z desce marcha (câmbio manual) |
| 3.1.6 | P1 | **Analógico direito** (4 direções) | Botões C: **cima = troca a visão**; nos menus navega as sub-telas (**Auto**: 4 direções e limiar de 40% no teste unitário) |
| 3.1.7 | P1 | Y | Atalho para C-cima (troca a visão) |
| 3.1.8 | P1 | Start | Pausa/continua |
| 3.1.9 | P2 | Direcional | Navega menus |
| 3.1.10 | P2 | LT/RT na seleção de carro e de pista | Z: troca a cor do carro / espelha a pista |

### 3.2 Esquema Moderno (PlayStation/Xbox)

Em **corrida ao vivo** (simulação rodando); **nos menus, na pausa e nas telas de resultado fica igual ao Clássico**.

| # | Pri | Botão (PlayStation / Xbox) | Esperado em corrida |
|---|---|---|---|
| 3.2.1 | P1 | R2 / RT | Acelera |
| 3.2.2 | P1 | L2 / LT | Freia (freio forte; parado, dá ré) |
| 3.2.3 | P1 | Quadrado / X | Desce marcha (câmbio manual) |
| 3.2.4 | P1 | X (Cruz) / A | **Sobe** marcha (câmbio manual) — e **não** acelera |
| 3.2.5 | P1 | Triângulo / Y | Troca a visão do carro |
| 3.2.6 | P2 | L1/R1 (LB/RB) | Repetem descer/subir marcha |
| 3.2.7 | P2 | Círculo / B | Sem função em corrida |
| 3.2.8 | P1 | **Transições** — largada (contagem), pausa → continuar, fim da corrida → telas de resultado → menu | Na **pausa** e nos **resultados**, X (Cruz)/A **confirma** (como no Clássico); ao continuar a corrida, o esquema moderno volta em ~0,3 s. **Se um botão ficar "no esquema errado", reporte onde** (o esquema é detectado lendo o estado do jogo) |
| 3.2.9 | P2 | Countdown | RT durante a contagem não deve "queimar a largada" de forma diferente do Clássico (A) |

### 3.3 Teclado, 2 jogadores e conexão

| # | Pri | Teste | Esperado |
|---|---|---|---|
| 3.3.1 | P1 | Teclado completo (tabela do §0) em corrida e menus | Tudo responde, incluindo I J K L = botões C |
| 3.3.2 | P2 | 2P Versus: P1 teclado/controle 1 e P2 teclado numérico (8/2/4/6, 0 = A, . = B, 7 = Start, 9 = Z, 1/3 = L/R) ou controle 2 | Os dois controlam o seu carro; o esquema (Clássico/Moderno) vale para os controles XInput dos dois |
| 3.3.3 | P2 | Desconectar e reconectar o controle durante a corrida/menu | Volta a responder sem reiniciar o jogo |
| 3.3.4 | P3 | Dois controles + teclado ao mesmo tempo | Sem entradas "fantasma" |

## 4. Vídeo e apresentação

| # | Pri | Teste | Esperado |
|---|---|---|---|
| 4.1 | P1 | **F11** (tela cheia sem bordas) várias vezes; minimizar e restaurar; arrastar/redimensionar | Sem congelar (era o bug da pintura GDI, corrigido), sem tela preta, imagem 4:3 centralizada com barras (**Auto**: estresse de 80 s, 0 congelamentos) |
| 4.2 | P1 | Fluidez a 120 Hz (seu monitor) em Enhanced | Movimento uniforme, sem engasgos periódicos (**Auto**: 93–99% dos quadros ficam exatamente 2 vblanks) |
| 4.3 | P1 | Rasgos de imagem (tearing) | Nenhum com v-sync ligado |
| 4.4 | P2 | `--vsync off` | Sem v-sync; com VRR/G-Sync não deve rasgar |
| 4.5 | P2 | `--filter nearest`, `bilinear`, `sharp` | Degraus / suave / nítido (**Auto**: conferido em captura) |
| 4.6 | P2 | Pausa › RESOLUTION: **HI-RES** e **NORM** | Alterna sem corromper a imagem nem em 2× |
| 4.7 | P2 | CAR VIEW: dentro com espelho (mirror), fora, dentro | Retrovisor **sem artefatos** em Enhanced (não testei o retrovisor em escala > 1) |
| 4.8 | P2 | GAMMA CORRECT on/off | Muda o brilho, sem listras |
| 4.9 | P2 | Efeitos: fumaça, faróis, neblina, pista molhada/noturna, névoa | Sem blocos pretos/brancos, sem "piscadas" de quadro (**Auto**: nenhum quadro intruso nas rotas, mas varie as pistas) |
| 4.10 | P3 | `--present gdi` | Funciona (sem v-sync) — só como fallback |

## 5. Áudio

| # | Pri | Teste | Esperado |
|---|---|---|---|
| 5.1 | P1 | Música e efeitos em menus e corrida, nos dois modos | Sem estalos/falhas, sincronizado com a imagem |
| 5.2 | P2 | MUSIC VOLUME e EFFECTS VOLUME | Mudam de fato |
| 5.3 | P2 | Sessão longa (30 min) | Áudio não atrasa em relação à imagem |
| 5.4 | P3 | Arrastar a janela / tela cheia durante o áudio | Sem cortes longos |

## 6. Modos do jogo (cobertura)

| # | Pri | Teste | Esperado |
|---|---|---|---|
| 6.1 | P1 | **Intro → título → demo → título → intro** (deixe rodar ~3 min) | Ciclo completo; a intro de Lisboa com o carro na largada, sem erros (**Auto**) |
| 6.2 | P1 | **Quick Race**: Car Select, Practice Laps, **Time Attack**, Arcade Race | Todos iniciam, terminam e voltam ao menu |
| 6.3 | P1 | **Campeonato**: criar piloto/equipe, aceitar oferta, abrir um evento, correr, ver resultados, **salvar e carregar** (Controller Pak) | Progressão e pontos corretos; o save carrega (**Auto**: pak validado contra a libultra original) |
| 6.4 | P1 | **GT2**: 10 copas com 2/2/3/3/3/3/4/5/5/15 voltas (a última, Invitational, Lisbon BR, 15 voltas) · **GT1**: 10 copas de 5 voltas, a última (Invitational, Kyoto CR) com 20 voltas | Número de voltas conforme o jogo (do seu relato e da ROM) |
| 6.5 | P1 | **Win Sequence** (zerar o jogo após a última corrida da GT1) | Toca o final **sem cair**. Conhecido: numa rota de teste (cheats + interruptores de depuração) o jogo caiu ao chegar à WDC WIN SEQUENCE; o código original, simulado a partir do mesmo estado, faz a mesma leitura do endereço 0, então **não** é defeito da recompilação, mas a causa exata não foi isolada e o fluxo normal (zerar a GT1) não foi testado até o fim. Se cair, **reporte** com o log |
| 6.6 | P2 | **Training** | Funciona |
| 6.7 | P2 | **2P Versus** (tela dividida), opções de 2P (voltas, carros IA, catch up, zerar placar) | Funciona; sem queda de fps |
| 6.8 | P2 | **Options**: Game Settings (música, efeitos, unidades de velocidade, display, transmissão, visão, gamma), View Replay, Records, Save Records, Credits | Tudo abre/grava; recordes mais altos dão mais pontos |
| 6.9 | P2 | **View Replay**: replay embutido e **replay salvo** (Load Replay) | Toca com trocas de câmera; um replay gravado em Enhanced pode divergir levemente da corrida real (re-simulado a 30 Hz — conhecido) |
| 6.10 | P2 | **Time Attack › Select Ghost** | Fantasma aparece e acompanha a volta |
| 6.11 | P2 | Aviso de pak ausente/danificado/cheio (rotas R6/R8/R9) | Mensagens corretas, sem travar (**Auto**: cobertas por rotas) |
| 6.12 | P2 | **Cheats** (os 22 códigos da ROM; ex.: `tracks`, `gt2 cars`, `gt1 cars`, `gt2 events`, `gt1 events`, `open win sequence`) digitados no menu principal | Desbloqueiam o que dizem (**Auto**: 12 cheats nomeados disparam) |
| 6.13 | P2 | **Z na seleção de pista** (pista espelhada) | Pista espelhada corretamente (Hawaii A testada) |

## 7. Jogabilidade e física (só você consegue)

| # | Pri | Teste | Esperado |
|---|---|---|---|
| 7.1 | P1 | Direção, frenagem, derrapagem, sensibilidade do volante em **Enhanced** vs **Legacy** | Mesma "sensação"; **este é o ponto de maior incerteza do 60 fps** (o jogo mistura `dt` com constantes por quadro) |
| 7.2 | P1 | IA: adversários largam, ultrapassam e erram como no original | Sem carros parados, "grudados" ou voando |
| 7.3 | P2 | Colisões com muros/carros, saída de pista, capotamento | Sem travar o carro no muro nem atravessar a geometria |
| 7.4 | P2 | Câmbio manual: subir/descer marchas e a ré (primeira marcha antes da ré) | Rotação e velocidade coerentes |
| 7.5 | P2 | Todas as pistas (Hawaii, Las Vegas, New Zealand, Lisbon, Rome, Sydney, Zurich, Kyoto, Black Forest, Les Gets) em corrida completa | Sem buracos na pista, sem objetos sumindo nas curvas, sem pop-in absurdo |
| 7.6 | P3 | Sol/neon/túneis: efeitos de luz e "flares" | Como no original |

## 8. Estabilidade (soak)

| # | Pri | Teste | Esperado |
|---|---|---|---|
| 8.1 | P1 | **Uma corrida de campeonato completa** (≥ 3 voltas) em Enhanced | Sem queda, sem congelar, sem `[hb] ATENCAO` no log |
| 8.2 | P2 | Deixar o jogo aberto 30–60 min (menus + corridas + demo) | Memória estável no Gerenciador de Tarefas (não cresce sem parar) |
| 8.3 | P2 | Alt-Tab, bloquear a tela, abrir outro app em tela cheia | Volta sem travar |
| 8.4 | P2 | Fechar com Esc e pelo X; reabrir | Salva o pak antes de fechar; reabre normal |
| 8.5 | P3 | Rodar duas vezes seguidas, trocar de modo no setup, rodar de novo | Sem arquivos corrompidos |

## 9. Já coberto automaticamente (para você não repetir)

- Tradução do código: 82,1% dos bytes de código comparados com o original por diferencial (≈ 370 000 segmentos sem divergência) em 12 rotas (telas, modos, resultados, 2P, pak).
- 60 fps: 60 tarefas gráficas/s medidas em corrida ao vivo; intro/demo/replay a 30 Hz idênticos ao Legacy; Time Attack com volta de 20,91 s (60 Hz) × 20,83 s (30 Hz) no mesmo roteiro.
- Apresentador D3D11: v-sync, filtros, tela cheia 2560×1600, minimizar/restaurar, 99% dos quadros com 2 vblanks a 120 Hz; 0 congelamentos em 80 s de estresse de janela e 5 min de sessão.
- Controles: 35 verificações do mapeamento (Clássico, Moderno em corrida, Moderno nos menus) passam.
- Setup: fluxo "escolher modo/controles → JOGAR" grava o `wdc_config.toml` e abre o jogo.

## 10. Itens que eu **sei** que ainda não foram testados (olhe com mais atenção)

1. Sensação de direção/física a 60 fps com um humano (§7.1).
2. Retrovisor (CAR VIEW com espelho), efeitos que leem o framebuffer e menus 2D em **escala interna > 1×**.
3. Replay gravado em Enhanced reproduzindo igual à corrida real.
4. Fantasma do Time Attack em Enhanced.
5. Escalas 4× e 8× com o apresentador.
6. Monitores de 144 Hz (o ritmo cai no padrão 2-3 de quadros) e vários monitores.
7. Esquema Moderno nas telas de resultado e na pausa **com um controle real** (a detecção "em corrida" lê o estado do jogo; só testei o mapeamento por testes unitários).

## 11. Leo Build 0.1.0 (canary) — o que testar nesta rodada (2026-11-12)

**P1 — antes de mandar para o seu pai**
1. **Instalação limpa**: copie `dist/WorldDriverChampionship-LeoBuild-0.1.0-canary-win64.zip` para OUTRO lugar (ou outro PC), extraia, abra `WorldDriverChampionship.exe` SEM a ROM por perto: deve aparecer a explicação e o seletor de arquivo; escolha a ROM (teste também uma ROM .v64/.n64 e um arquivo errado: deve avisar em português e deixar tentar de novo). Na 2ª abertura não pode pedir a ROM. Sem janela de console.
2. **Setup → IMAGEM…**: troque AUTO/1×/2×/4×, 4:3 × ESTICAR, os 4 tipos de suavização (compare NÍTIDO × BICÚBICO com o carro parado na seleção de carros), V-Sync, "ao abrir: tela cheia". Aperte JOGAR e veja se cada escolha valeu (a resolução só vale em Enhanced). 4× numa placa integrada pode pesar: é esperado (AUTO já escolhe 2× nela).
3. **Setup → PERSONALIZAR…**: no Clássico e no Moderno, mude acelerar/frear/marchas/visão com o controle (clique na caixa do meio e aperte o botão) e jogue uma volta; "Padrão" volta ao original. Confira que a duplicata é removida (ao dar a mesma tecla a duas ações, a primeira fica "SEM BOTÃO").
4. **Atalho na área de trabalho** (botão no topo do setup): cria e abre o jogo.
5. **Piscadas (Enhanced)**: jogue 10–15 min em Enhanced (menus, seleção de carro, contagem e corrida): os quadros com céu/fundo verde "chiando" não devem mais aparecer. Se aparecer algum, anote onde (tela, carro, pista) e me diga.
6. **Áudio (Enhanced)**: faça uma corrida com o motor no talo e colisões; o som deve estar limpo (o atraso de leitura que testei foi desfeito porque fazia o som picotar). Se ainda houver estalos/"explosões", anote a pista, o carro e o que acontecia (colisão, motor no talo) e me diga.

**P2**: latência do controle em Enhanced (+1 quadro pelo filtro de piscadas; desliga com `WDC_NO_DEGLITCH=1`); saves de Legacy × Enhanced continuam separados.
