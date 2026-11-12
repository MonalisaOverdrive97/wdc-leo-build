"""Gera os arquivos de entrada do N64Recomp (modo "symbols file + ROM") para o WDC.

Entrada : config/asm/1000.s e config/asm/1050.s (saida do splat) + rom/wdc_bigendian.z64
Saida   : recomp/wdc.syms.toml  (secao unica + lista de funcoes)
          recomp/wdc.toml       (config do N64Recomp)

O splat tratou o microcodigo RSP que comeca em ROM 0x7D2B0 como codigo da CPU
(func_8007C6B0 e func_8007C6B8). O recompilador so pode receber codigo de CPU,
entao tudo a partir de CPU_TEXT_END e descartado aqui.

Uso: python tools/recomp/gen_wdc_toml.py [--trace]
  (padrao): recomp/wdc.toml -> RecompiledFuncs/, com os hooks condicionais do modo Enhanced (ENH_HOOKS; inertes no Legacy).
  --trace : gera recomp/wdc_trace.toml (saida em RecompiledFuncsTrace/, com trace_mode) para o mapa de cobertura.
"""
import bisect
import json
import re
import struct
import sys
from pathlib import Path

import rabbitizer

TRACE = "--trace" in sys.argv
ROOT = Path(__file__).resolve().parents[2]
ASM_FILES = [ROOT / "config" / "asm" / "1000.s", ROOT / "config" / "asm" / "1050.s"]
ROM_PATH = ROOT / "rom" / "wdc_bigendian.z64"
OUT_DIR = ROOT / "recomp"

ROM_BASE = 0x1000        # ROM offset do entrypoint (carregado pelo IPL3)
VRAM_BASE = 0x80000400   # endereco virtual do entrypoint
CPU_TEXT_END = 0x7D2B0   # ROM offset onde comeca o microcodigo RSP

# Nomes de libultra revisados (novos, corrigidos e removidos) com a evidencia de cada um.
NAMES_JSON = ROOT / "tools" / "recomp" / "libultra_names.json"
SYMBOL_LISTS = ROOT / "thirdparty" / "N64Recomp" / "src" / "symbol_lists.cpp"

# Sub-rotinas internas do tratador de excecoes (exceptasm) que o splat separou como funcao
# propria. Todos os 14 jal para func_80075474 vem de dentro de __osExceptionPreamble (que o
# N64Recomp ja ignora); o runtime trata interrupcoes, esse codigo nao deve ser recompilado.
# Os nomes de "ignorar" em libultra_names.json sao somados a esta lista.
EXTRA_IGNORED = ["func_80075474"]

# Unicos ops de `cache` presentes no codigo da Boss (fora de libultra). Nomes segundo as
# constantes do R4300.h da libultra (fora do manual Intro to the N64); ambos sao dicas de
# coerencia de cache, sem efeito em um espaco de memoria plano.
# Correcoes de ORDEM DE ESCALONAMENTO. O escalonador do runtime (ultramodern/src/scheduling.cpp) e cooperativo: o criador
# de uma thread so cede a CPU em pontos de escalonamento. No N64 real um tick de VI pendente teria sido tratado por
# preempcao (antes de a fila de destino existir); aqui ele so e entregue no primeiro ponto de escalonamento, que e o
# proprio osStartThread, e acorda a thread nova ANTES do codigo que vem depois dele no criador.
#   func_8002CB0C (chamada no RACEINIT): cria a fila 0x800B7600 e a thread 9 (func_8002C500, prioridade 11), da
#   osStartThread e SO ENTAO inicializa os 8 slots de controle (func_8002C494 grava pad+0x74). Se a thread 9 acordar
#   antes do laco, func_80002F58 le `lw v0,0x74(pad)` = NULL e `lbu 4(NULL)` derruba o processo (visto em ~60% das
#   execucoes). Correcao: o `jal osStartThread` vira nop e a chamada e refeita no fim da funcao (antes do epilogo).
#   Observado em execucao com tracos de fila/thread (trace_hooks.cpp); o resultado do programa nao muda: a thread 9
#   apenas bloqueia em osRecvMesg na primeira vez.
SCHED_PATCHES = [
    {
        "func": "func_8002CB0C",
        "instr": [(0x8002CCF4, "jal", 0x8006F820, "osStartThread")],        # (endereco, tipo esperado, alvo esperado, nome)
        "hooks": [(0x8002CD20, "ctx->r4 = S32(0x800B7620u); osStartThread_recomp(rdram, ctx);")],
    },
]

# Cadeias de retorno por $ra COMPUTADO (biblioteca de vetores em assembly da Boss). O corpo faz `or $t9,$ra,$zero`, carrega em $ra o
# endereco de uma "cauda" (`jr $t9 ; sqrt.s $f0,$f0`) e cai no calculo do produto escalar; o `jr $ra` do corpo salta para a cauda, que
# tira a raiz e volta ao chamador via $t9. O N64Recomp traduz `jr $ra` como retorno do C, entao a cauda NUNCA rodava: as quatro funcoes
# devolviam o comprimento/distancia AO QUADRADO (achado pelo diferencial de CPU por segmentos: normalizar um vetor dividia por len^2 e
# produzia vetores ~8x menores => matrizes/camera/culling errados). Correcao: o `jr $ra` do corpo vira `j <cauda>` (chamada final) e o
# `jr $t9` da cauda vira `jr $ra` (o retorno real ao chamador do C).
#   (funcao do corpo, pc do `jr $ra` do corpo, nome da cauda, pc da cauda)
TAIL_RA_PATCHES = [
    ("func_80019FC0", 0x80019FE8, "func_80019FF0", 0x80019FF0),   # |v| 3D
    ("func_8001A220", 0x8001A260, "func_8001A268", 0x8001A268),   # distancia 3D
    ("func_8001A574", 0x8001A5A0, "func_8001A5A8", 0x8001A5A8),   # distancia 2D
    ("func_8001A5C0", 0x8001A5E0, "func_8001A5E8", 0x8001A5E8),   # |v| 2D
]

# Funcoes cujo corpo vira `return` (invalidar/escrever de volta cache de dados e irrelevante no host). Cada entrada e conferida no
# gerador: precisa existir entre as funcoes mantidas.
STUB_FUNCS = ["func_8002D7F0"]

# MODO ENHANCED (60 fps): hooks CONDICIONAIS no C gerado. Cada um so age com a chave `g_wdc_enh_fps60` (preenchida por main.cpp a partir de wdc_config:
# mode = "enhanced" e enhanced.fps60); com a chave em 0 (Legacy) o C executa exatamente o codigo original. Nao sao emitidos na build --trace (so Legacy).
#
# Limitador 1: o laco de quadro func_800013DC espera DOIS retracos por quadro: osRecvMesg(VI) -> osViSwapBuffer -> osRecvMesg(VI) -> osViGetCurrentFramebuffer
# (se o buffer exibido ainda nao e o novo, espera outro). No Enhanced: dispensa a 2a espera (g_wdc_skip_recv arma wdc_blk_osRecvMesg para devolver na hora)
# e forca a comparacao a "igual" (beq $v0,$t0).
# Limitador 2 (achado por WDC_MSGLOG no build trace): a thread de entrada func_8002C500 descarta `n` retracos de VI entre dois quadros processados, com n =
# 2o argumento de func_8002CB0C(callback, n) guardado em 0x8007FE1C; o menu usa n=0 (60 Hz) e a inicializacao de corrida (func_800369E0, 0x80037B44) usa
# n=1 => 30 Hz so em corrida. O dt do jogo NAO e medido: func_8002CB0C grava em [0x8007FDF4] o periodo nominal (n+1) x 0xBEBC2 ticks (1/60 s por retraco) e
# func_8002C0B8 o acumula a cada quadro processado, entao o hook em func_8002C500 (antes de `lui $s0,0x8008`, 0x8002C584) regrava n e o periodo a cada ciclo.
#
# DEMO E REPLAY sao RE-SIMULACOES de amostras de entrada gravadas a 30 Hz (1 byte de botoes + 1 de direcao por iteracao do laco). A 60 Hz a fisica difere um
# pouco da de 30 Hz (o jogo mistura dt com constantes por iteracao): o carro do demo da abertura saia errado. Decisao: em reproducao ([0x8007DF94] == 2) o
# laco volta a 30 Hz (n=1, periodo 2x, igual ao original); ao vivo, 60 Hz. A GRAVACAO (func_80003DA4, modo 1) amostra a 30 Hz (iteracoes impares so mantem o
# resultado), para os replays continuarem no formato original (wdc_demo_pre/post em recomp/runtime_glue/wdc_runtime_stubs.cpp).
#   (funcao, pc, palavra original esperada, texto)
ENH_HOOKS = [
    ("func_800013DC", 0x800015BC, 0x0C01BB5C, "g_wdc_skip_recv = g_wdc_enh_fps60;"),   # antes do `jal osRecvMesg` da 2a espera
    ("func_800013DC", 0x800015D4, 0x10480006, "if (g_wdc_enh_fps60) ctx->r2 = ctx->r8;"),   # antes do `beq $v0,$t0`: buffer exibido == novo
    ("func_8002C500", 0x8002C584, 0x3C108008, "if (g_wdc_enh_fps60) { int32_t wdc_n = (MEM_W(-0X206C, S32(0X8008 << 16)) == 2) ? 1 : 0; MEM_W(-0X1E4, S32(0X8008 << 16)) = wdc_n; MEM_W(-0X20C, S32(0X8008 << 16)) = 0xBEBC2 * (wdc_n + 1); }"),
    ("func_80003DA4", 0x80003DA4, 0x27BDFFD0, "if (g_wdc_enh_fps60 && wdc_demo_pre(rdram, ctx, 0)) return;"),
    ("func_80003DA4", 0x80003E6C, 0x03E00008, "if (g_wdc_enh_fps60) wdc_demo_post(rdram, ctx, 0);"),
]

# Sondas de diagnostico (SO na build --trace): [[patches.hook]] que chamam wdc_probe_poly()/wdc_exp() de recomp/project/src/trace_hooks.cpp.
# func_8002703C = laco de poligonos da CPU (recorte/ordenacao por profundidade). 0x80027158 = logo antes do descarte trivial por flags do RSP
# (bnel $t8,$zero: t8 = AND das flags +0xC dos 4 vertices).
TRACE_PROBES = [
    # caminho de TRIANGULOS do laco de poligonos: S2 descarte por flags do RSP (+0xC), S3 descarte por profundidade, S4 retorno do recortador
    # (func_80026208), S6 descarte de face traseira, S7 emissao sem recorte.  Experimento 1 (WDC_EXP=1): ignora o descarte por flags.
    ("func_8002703C", 0x80027558, "if (wdc_exp(1)) ctx->r25 = 0; wdc_probe_poly(rdram, ctx, 2);"),
    ("func_8002703C", 0x80027588, "wdc_probe_poly(rdram, ctx, 3);"),
    ("func_8002703C", 0x800275CC, "wdc_probe_poly(rdram, ctx, 4);"),
    ("func_8002703C", 0x80027630, "wdc_probe_poly(rdram, ctx, 6);"),
    ("func_8002703C", 0x80027638, "wdc_probe_poly(rdram, ctx, 7);"),
    # S8: logo antes do `jal func_80026208` (recortador): captura do estado para o teste diferencial contra um emulador MIPS (WDC_SNAP)
    ("func_8002703C", 0x800275C4, "wdc_probe_poly(rdram, ctx, 8);"),
    # S9/S10: entrada e saida da passagem de poligonos func_8002784C (so chama func_8002703C/recortador; sem funcoes do SO): teste diferencial de um quadro inteiro
    # S11: caminho de QUADRILATEROS, logo antes do descarte por flags (r24 = AND das flags dos 4 vertices)
    ("func_8002703C", 0x80027158, "wdc_probe_poly(rdram, ctx, 11);"),
    ("func_8002784C", 0x8002784C, "wdc_probe_poly(rdram, ctx, 9);"),
    ("func_8002784C", 0x8002791C, "wdc_probe_poly(rdram, ctx, 10);"),
]

CACHE_NOP_OPS = {
    0x0D: "Create Dirty Exclusive (D)",  # seguido de sd que preenchem a linha de 16 bytes
    0x11: "Hit Invalidate (D)",
}

INSN_RE = re.compile(r"/\*\s*([0-9A-Fa-f]+)\s+([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{8})\s*\*/")
GLABEL_RE = re.compile(r"^glabel\s+(\S+)")
ENDLABEL_RE = re.compile(r"^endlabel\s+(\S+)")


def parse_functions():
    """Devolve [(nome, rom, vram, n_palavras)] na ordem em que aparecem."""
    funcs = []
    for path in ASM_FILES:
        cur = None
        for line in path.read_text(encoding="utf-8").splitlines():
            m = GLABEL_RE.match(line)
            if m:
                cur = {"name": m.group(1), "rom": None, "vram": None, "n": 0}
                continue
            if cur is not None:
                m = INSN_RE.search(line)
                if m:
                    if cur["rom"] is None:
                        cur["rom"] = int(m.group(1), 16)
                        cur["vram"] = int(m.group(2), 16)
                    cur["n"] += 1
                    continue
                m = ENDLABEL_RE.match(line)
                if m and m.group(1) == cur["name"]:
                    funcs.append((cur["name"], cur["rom"], cur["vram"], cur["n"]))
                    cur = None
    return funcs


def apply_names(kept):
    """Aplica libultra_names.json validando cada entrada contra o nome atual do splat."""
    data = json.loads(NAMES_JSON.read_text(encoding="utf-8"))
    new_names = {int(k, 16): v for k, v in data["nomes"].items()}
    removals = {int(k, 16): v for k, v in data["remover"].items()}
    errors, out, changes = [], [], {"novo": 0, "corrige": 0, "remover": 0}
    for name, rom_off, vram, n in kept:
        anon = name.startswith("func_")
        if vram in new_names:
            new, kind, why = new_names[vram]
            if kind == "novo" and not anon:
                errors.append(f"0x{vram:08X}: '{new}' marcado 'novo' mas a funcao ja se chama {name}")
            if kind == "corrige" and anon:
                errors.append(f"0x{vram:08X}: '{new}' marcado 'corrige' mas a funcao e anonima ({name})")
            changes[kind] += 1
            name = new
        elif vram in removals:
            old = removals[vram][0]
            if name != old:
                errors.append(f"0x{vram:08X}: remover '{old}' mas a funcao se chama {name}")
            changes["remover"] += 1
            name = f"func_{vram:08X}"
        out.append((name, rom_off, vram, n))
    seen = {}
    for name, _, vram, _ in out:
        if name in seen and not name.startswith("func_"):
            errors.append(f"nome duplicado '{name}' em 0x{seen[name]:08X} e 0x{vram:08X}")
        seen[name] = vram
    found = {f[2] for f in out}
    for vram in list(new_names) + list(removals):
        if vram not in found:
            errors.append(f"0x{vram:08X}: endereco de libultra_names.json nao e inicio de funcao")
    return out, set(data.get("ignorar", {})), changes, errors


def main():
    rom = ROM_PATH.read_bytes()
    funcs = parse_functions()
    total = len(funcs)
    kept = [f for f in funcs if f[1] < CPU_TEXT_END]
    dropped = [f for f in funcs if f[1] >= CPU_TEXT_END]
    kept.sort(key=lambda f: f[2])
    kept, json_ignored, name_changes, name_errors = apply_names(kept)
    if name_errors:
        print("ERRO em libultra_names.json:")
        for e in name_errors:
            print("  " + e)
        return 1
    ignored_cfg = EXTRA_IGNORED + sorted(json_ignored)
    listed = set(re.findall(r'"([A-Za-z0-9_]+)"', SYMBOL_LISTS.read_text(encoding="utf-8")))
    skipped = listed | set(ignored_cfg)   # nao sao recompiladas, entao nao ha o que alertar

    starts = {f[2] for f in kept}
    ends = {f[2] + f[3] * 4 for f in kept}
    problems = []

    # Cada funcao precisa caber na ROM e a mapeamento linear rom<->vram tem que valer.
    for name, rom_off, vram, n in kept:
        if vram - VRAM_BASE + ROM_BASE != rom_off:
            problems.append(f"{name}: mapeamento rom/vram inconsistente (rom 0x{rom_off:X} vram 0x{vram:08X})")
        if rom_off + n * 4 > len(rom):
            problems.append(f"{name}: fora dos limites da ROM")

    # Instrucoes invalidas e alvos de jal/j que nao caem no inicio de uma funcao.
    invalid, bad_targets = [], []
    for name, rom_off, vram, n in kept:
        for i in range(n):
            w = struct.unpack(">I", rom[rom_off + i * 4: rom_off + i * 4 + 4])[0]
            pc = vram + i * 4
            if not rabbitizer.Instruction(w, vram=pc).isValid():
                invalid.append((name, pc, w))
            op = w >> 26
            if op in (2, 3):  # j / jal
                tgt = ((pc + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
                if VRAM_BASE <= tgt < VRAM_BASE + (CPU_TEXT_END - ROM_BASE) and tgt not in starts:
                    bad_targets.append((name, pc, tgt, "jal" if op == 3 else "j"))

    # O N64Recomp so fecha o "}" do C no fim de uma funcao: se ela cai (fall-through) na
    # seguinte sem jr/j/b, o corpo seguinte nunca e executado. Rotinas escritas a mao pela
    # Boss fazem isso de proposito (ex.: func_8001A000 ajusta argumentos e cai em func_8001A008).
    # Solucao: a funcao "ponte" e emitida ja estendida com o corpo seguinte (que continua
    # existindo como funcao propria). O C fica duplicado, mas com a semantica correta.
    def terminates(rom_off, n):
        if n >= 2:
            prev = struct.unpack(">I", rom[rom_off + (n - 2) * 4: rom_off + (n - 1) * 4])[0]
            if prev >> 26 == 0 and (prev & 0x3F) == 0x08:      # jr
                return True
            if prev >> 26 == 2:                                # j
                return True
            if (prev & 0xFFFF0000) == 0x10000000:              # b (beq $zero, $zero)
                return True
        last = struct.unpack(">I", rom[rom_off + (n - 1) * 4: rom_off + n * 4])[0]
        return last == 0x42000018                              # eret (sem delay slot)

    # Mesmo problema com desvios (b/beq/bne...) para o MEIO de outra funcao: o N64Recomp so
    # aceita saltar para fora quando o alvo e o inicio de uma funcao (tail call). Aqui a
    # funcao e estendida ate o fim da funcao que contem o alvo; o calculo e em ponto fixo
    # porque a extensao pode trazer novos desvios/fall-through para dentro da faixa.
    by_start = {f[2]: f for f in kept}
    ostarts = [f[2] for f in kept]
    oends = [f[2] + f[3] * 4 for f in kept]
    nat_term = {f[2]: terminates(f[1], f[3]) for f in kept}
    ext_end = {f[2]: f[2] + f[3] * 4 for f in kept}
    reasons = {f[2]: set() for f in kept}
    escapes_back = []

    def containing(addr):
        i = bisect.bisect_right(ostarts, addr) - 1
        if i >= 0 and addr < oends[i]:
            return ostarts[i]
        return None

    def word_at(pc):
        o = pc - VRAM_BASE + ROM_BASE
        return struct.unpack(">I", rom[o:o + 4])[0]

    changed = True
    while changed:
        changed = False
        for name, rom_off, vram, n in kept:
            end = ext_end[vram]
            new_end = end
            nat_end = vram + n * 4
            if not nat_term[vram] and nat_end in by_start:
                if ext_end[nat_end] > new_end:
                    new_end = ext_end[nat_end]
                    reasons[vram].add("fall-through")
            for pc in range(vram, end, 4):
                w = word_at(pc)
                if not rabbitizer.Instruction(w, vram=pc).isBranch():
                    continue
                imm = w & 0xFFFF
                tgt = pc + 4 + (imm - 0x10000 if imm & 0x8000 else imm) * 4
                if tgt in by_start or vram <= tgt < end:
                    continue
                g = containing(tgt) if tgt >= vram else None
                if g is None:
                    escapes_back.append((name, pc, tgt))
                elif ext_end[g] > new_end:
                    new_end = ext_end[g]
                    reasons[vram].add("desvio")
            if new_end != end:
                ext_end[vram] = new_end
                changed = True

    ext_words = {v: (e - v) // 4 for v, e in ext_end.items()}
    escapes_back = sorted({e for e in escapes_back if e[0] not in skipped})
    extended = [(f[0], ext_words[f[2]] * 4, sorted(reasons[f[2]])) for f in kept if ext_words[f[2]] != f[3]]
    dangling = [(f[0], f[2] + f[3] * 4) for f in kept
                if f[0] not in skipped and not nat_term[f[2]] and (f[2] + f[3] * 4) not in by_start]

    # O N64Recomp nao implementa `cache`. Nas funcoes de libultra isso nao importa (o
    # recompilador as renomeia e o runtime as substitui). No codigo da Boss todo `cache` e
    # 0x0D seguido de `sd` que preenchem a linha de 16 bytes inteira, entao um nop mantem
    # o comportamento. Funcoes das listas do N64Recomp ficam de fora: renomeadas para
    # <nome>_recomp, um patch por nome falharia ("does not exist").
    cache_patches, cache_other_ops = [], []
    for name, rom_off, vram, n in kept:
        if name in skipped:
            continue
        for i in range(ext_words[vram]):
            w = struct.unpack(">I", rom[rom_off + i * 4: rom_off + i * 4 + 4])[0]
            if w >> 26 == 0x2F:
                if (w >> 16) & 0x1F not in CACHE_NOP_OPS:
                    cache_other_ops.append((name, vram + i * 4, w))
                cache_patches.append((name, vram + i * 4))

    section_size = CPU_TEXT_END - ROM_BASE
    OUT_DIR.mkdir(exist_ok=True)

    lines = [
        "# Gerado por tools/recomp/gen_wdc_toml.py -- nao editar a mao.",
        "# Secao unica de codigo de CPU: rom 0x1000 -> vram 0x80000400 (mapeamento linear).",
        "[[section]]",
        'name = "main"',
        f"rom = 0x{ROM_BASE:X}",
        f"vram = 0x{VRAM_BASE:08X}",
        f"size = 0x{section_size:X}",
        "functions = [",
    ]
    for name, rom_off, vram, n in kept:
        lines.append(f'    {{ name = "{name}", vram = 0x{vram:08X}, size = 0x{ext_words[vram] * 4:X} }},')
    lines.append("]")
    (OUT_DIR / "wdc.syms.toml").write_text("\n".join(lines) + "\n", encoding="utf-8")

    cfg = [
        "# Config do N64Recomp para World Driver Championship (NWOE).",
        "# Rodar a partir desta pasta:  N64RecompCLI wdc.toml",
        "[input]",
        "entrypoint = 0x80000400",
        'symbols_file_path = "wdc.syms.toml"',
        'rom_file_path = "../rom/wdc_bigendian.z64"',
        'output_func_path = "RecompiledFuncsTrace"' if TRACE else 'output_func_path = "RecompiledFuncs"',
        *(["trace_mode = true            # TRACE_ENTRY()/TRACE_RETURN() em cada funcao (macros em recomp/project/src/trace.h)"] if TRACE else []),
        "",
        "[patches]",
        "# O main() do jogo (0x800028E0) colidiria com o main() do runtime: vira main_recomp.",
        'renamed = ["main"]',
        "# Partes do tratador de excecoes da libultra: o runtime cuida das interrupcoes.",
        "ignored = [" + ", ".join(f'"{n}"' for n in ignored_cfg) + "]",
        "# Rotinas de cache de dados do jogo (sem efeito no host, que e coerente). func_8002D7F0 = invalidacao de D-cache com `jr $a1` para",
        "# dentro de uma sequencia desenrolada de `cache 0x11` (dispositivo de Duff): o N64Recomp nao resolve esse salto e o runtime aborta",
        "# em get_function(0x8002D85C). Trocada por um retorno vazio.",
        "stubs = [" + ", ".join(f'"{n}"' for n in STUB_FUNCS) + "]",
    ]
    for name, pc in cache_patches:
        cfg += [
            "",
            "[[patches.instruction]]",
            f'func = "{name}"',
            f"vram = 0x{pc:08X}",
            "value = 0x00000000  # cache -> nop",
        ]
    # Patches de ordem de escalonamento (SCHED_PATCHES): valida a palavra original antes de trocar por nop.
    kept_names = {k[0] for k in kept}
    for sp in SCHED_PATCHES:
        if sp["func"] not in kept_names or sp["func"] in skipped:
            raise SystemExit(f"SCHED_PATCHES: funcao {sp['func']} nao esta entre as mantidas")
        for pc, kind, target, nm in sp["instr"]:
            w = struct.unpack(">I", rom[pc - VRAM_BASE + ROM_BASE: pc - VRAM_BASE + ROM_BASE + 4])[0]
            want = 0x0C000000 | (target >> 2) & 0x03FFFFFF
            if kind != "jal" or w != want:
                raise SystemExit(f"SCHED_PATCHES: 0x{pc:08X} tem 0x{w:08X}, esperado jal 0x{target:08X} ({nm})")
            cfg += ["", "[[patches.instruction]]", f'func = "{sp["func"]}"', f"vram = 0x{pc:08X}",
                    f"value = 0x00000000  # jal {nm} -> nop (refeito por hook mais adiante)"]
        for pc, text in sp["hooks"]:
            cfg += ["", "[[patches.hook]]", f'func = "{sp["func"]}"', f"before_vram = 0x{pc:08X}", f'text = "{text}"']
    # Cadeias de retorno por $ra computado (TAIL_RA_PATCHES): valida as palavras originais e troca.
    for body, jr_pc, tail, tail_pc in TAIL_RA_PATCHES:
        if body not in kept_names or tail not in kept_names:
            raise SystemExit(f"TAIL_RA_PATCHES: {body}/{tail} nao estao entre as mantidas")
        w_body = struct.unpack(">I", rom[jr_pc - VRAM_BASE + ROM_BASE: jr_pc - VRAM_BASE + ROM_BASE + 4])[0]
        w_tail = struct.unpack(">I", rom[tail_pc - VRAM_BASE + ROM_BASE: tail_pc - VRAM_BASE + ROM_BASE + 4])[0]
        if w_body != 0x03E00008 or w_tail != 0x03200008:   # jr $ra / jr $t9
            raise SystemExit(f"TAIL_RA_PATCHES: palavras inesperadas em 0x{jr_pc:08X} (0x{w_body:08X}) ou 0x{tail_pc:08X} (0x{w_tail:08X})")
        j_word = 0x08000000 | ((tail_pc & 0x0FFFFFFF) >> 2)
        cfg += ["", "[[patches.instruction]]", f'func = "{body}"', f"vram = 0x{jr_pc:08X}",
                f"value = 0x{j_word:08X}  # jr $ra -> j {tail} (a cauda tira a raiz; ver TAIL_RA_PATCHES)"]
        cfg += ["", "[[patches.instruction]]", f'func = "{tail}"', f"vram = 0x{tail_pc:08X}",
                "value = 0x03E00008  # jr $t9 -> jr $ra (retorno real ao chamador)"]
    # Codigo automodificavel: func_80037F20 (chamado em func_800380A4 no boot) reescreve marcadores `xori/andi/ori ..., 0xNBFD` de
    # tres rotinas de poligonos como `cache 0x0D` (Create Dirty Exclusive). Sem isto o C gerado executa os marcadores crus (um deles
    # destroi o ponteiro do bloco => queda em func_80026208). Reproduzido offline por tools/recomp/runtime_code_patcher.py.
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import runtime_code_patcher
    rt_patches = runtime_code_patcher.compute_patches(rom)
    kept_by_start = sorted((v, ext_words[v] * 4, nm) for nm, _o, v, _n in kept if nm not in skipped)
    def func_at(addr):
        i = bisect.bisect_right([k[0] for k in kept_by_start], addr) - 1
        if i >= 0 and kept_by_start[i][0] <= addr < kept_by_start[i][0] + kept_by_start[i][1]:
            return kept_by_start[i][2]
        raise SystemExit(f"runtime_code_patcher: 0x{addr:08X} fora das funcoes mantidas")
    already = {pc for _n, pc in cache_patches}
    rt_count = 0
    for addr, (old, new) in rt_patches.items():
        if new >> 26 == 0x2F:                       # cache
            if (new >> 16) & 0x1F not in CACHE_NOP_OPS:
                raise SystemExit(f"runtime_code_patcher: cache com op desconhecido em 0x{addr:08X}")
            value, why = 0, "cache -> nop"
        else:
            value, why = new, "palavra final do patcher"
        if addr in already:
            continue
        cfg += ["", "[[patches.instruction]]", f'func = "{func_at(addr)}"', f"vram = 0x{addr:08X}",
                f"value = 0x{value:08X}  # marcador 0x{old:08X} reescrito em tempo de execucao por func_80037F20 ({why})"]
        rt_count += 1
    print(f"palavras reescritas pelo patcher de codigo do jogo (func_80037F20): {rt_count}")
    if TRACE:
        for fn, pc, text in TRACE_PROBES:
            cfg += ["", "[[patches.hook]]", f'func = "{fn}"', f"before_vram = 0x{pc:08X}", f'text = "{text}"']
    if not TRACE:
        for fn, pc, old, text in ENH_HOOKS:
            w = struct.unpack(">I", rom[pc - VRAM_BASE + ROM_BASE: pc - VRAM_BASE + ROM_BASE + 4])[0]
            if w != old:
                raise SystemExit(f"ENH_HOOKS: 0x{pc:08X} tem 0x{w:08X}, esperado 0x{old:08X}")
            cfg += ["", "[[patches.hook]]", f'func = "{fn}"', f"before_vram = 0x{pc:08X}", f'text = "{text}"']
    cfg_name = "wdc_trace.toml" if TRACE else "wdc.toml"
    (OUT_DIR / cfg_name).write_text("\n".join(cfg) + "\n", encoding="utf-8")

    print(f"funcoes no splat: {total}  mantidas (CPU): {len(kept)}  descartadas (RSP): {len(dropped)}")
    print(f"nomes de libultra aplicados: {name_changes['novo']} novos, {name_changes['corrige']} corrigidos, {name_changes['remover']} removidos")
    print(f"funcoes nomeadas agora: {sum(1 for f in kept if not f[0].startswith('func_'))}   anonimas: {sum(1 for f in kept if f[0].startswith('func_'))}")
    for name, rom_off, vram, n in dropped:
        print(f"   descartada {name} rom 0x{rom_off:X} ({n * 4} bytes)")
    print(f"secao main: rom 0x{ROM_BASE:X} vram 0x{VRAM_BASE:08X} size 0x{section_size:X}")
    print(f"instrucoes invalidas (CPU): {len(invalid)}")
    for name, pc, w in invalid[:15]:
        print(f"   {name} @0x{pc:08X} palavra 0x{w:08X}")
    print(f"jal/j para alvo que nao e inicio de funcao: {len(bad_targets)}")
    for name, pc, tgt, kind in bad_targets[:25]:
        print(f"   {name} @0x{pc:08X} {kind} 0x{tgt:08X}")
    print(f"funcoes estendidas (fall-through ou desvio para o meio de outra funcao): {len(extended)}")
    for name, size, why in extended:
        print(f"   {name:18s} tamanho emitido 0x{size:X}  ({', '.join(why)})")
    print(f"desvios que escapam da funcao sem como resolver por extensao: {len(escapes_back)}")
    for name, pc, tgt in escapes_back[:20]:
        print(f"   {name} @0x{pc:08X} -> 0x{tgt:08X}")
    print(f"funcoes que caem no vazio (sem funcao seguinte): {len(dangling)}")
    for name, end in dangling:
        print(f"   {name} termina em 0x{end:08X} sem jr/j/b")
    print(f"`cache` trocados por nop (codigo da Boss): {len(cache_patches)} em {len({p[0] for p in cache_patches})} funcoes")
    print(f"`cache` com op fora de CACHE_NOP_OPS (precisam de analise): {len(cache_other_ops)}")
    for name, pc, w in cache_other_ops[:10]:
        print(f"   {name} @0x{pc:08X} palavra 0x{w:08X}")
    print(f"inconsistencias de mapeamento: {len(problems)}")
    for p in problems[:10]:
        print("   " + p)
    print(f"escrito: {OUT_DIR / 'wdc.syms.toml'}")
    print(f"escrito: {OUT_DIR / cfg_name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
