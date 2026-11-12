"""Gera recomp/project/src/wdc_seg_hooks.h e wdc_seg_wrappers.cpp (build TRACE): fronteiras dos "segmentos" do diferencial de CPU.

Fronteira = chamada de uma funcao que o RUNTIME implementa (sem corpo no C gerado: `rom_size = 0` em recomp_overlays.inl, mais os
stubs do glue chamados direto, p.ex. rmonPrintf). O codigo da CPU entre duas fronteiras de uma thread e puro (so CPU+memoria) e pode ser
reexecutado por um emulador MIPS (tools/recomp/difftest_seg.py). Cada fronteira ganha um wrapper `wdc_seg_<nome>` que registra o estado
(trace_seg.cpp: wdc_seg_call) e chama a funcao real (ou o hook existente do projeto: wdc_hook_*).

Uso: python tools/recomp/gen_seg_hooks.py      (depois de gerar o C da build trace)
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GEN = ROOT / "recomp" / "RecompiledFuncsTrace"
SYMS = ROOT / "recomp" / "wdc.syms.toml"
OUT_H = ROOT / "recomp" / "project" / "src" / "wdc_seg_hooks.h"
OUT_CPP = ROOT / "recomp" / "project" / "src" / "wdc_seg_wrappers.cpp"

# nomes que ja tem hook do projeto (wdc_hooks.h / trace_hooks.h): o wrapper de segmento chama o hook, nao o runtime direto
HOOKED = {
    "osSetEventMesg": "wdc_hook_osSetEventMesg", "osSpTaskLoad": "wdc_hook_osSpTaskLoad", "osSpTaskYield": "wdc_hook_osSpTaskYield",
    "osSpTaskStartGo": "wdc_hook_osSpTaskStartGo", "osRecvMesg": "wdc_hook_osRecvMesg", "osSendMesg": "wdc_hook_osSendMesg",
    "osJamMesg": "wdc_hook_osJamMesg", "osCreateThread": "wdc_hook_osCreateThread", "osStartThread": "wdc_hook_osStartThread",
    "osPfsInitPak": "wdc_hook_osPfsInitPak",
    "osPfsRepairId": "wdc_hook_osPfsRepairId",
    "osPfsFileState": "wdc_hook_osPfsFileState",
    "osPfsFindFile": "wdc_hook_osPfsFindFile",
    "osPfsAllocateFile": "wdc_hook_osPfsAllocateFile",
    "osPfsDeleteFile": "wdc_hook_osPfsDeleteFile",
    "osPfsReadWriteFile": "wdc_hook_osPfsReadWriteFile",
    "osPfsFreeBlocks": "wdc_hook_osPfsFreeBlocks",
    "osPfsNumFiles": "wdc_hook_osPfsNumFiles",
    "osPfsChecker": "wdc_hook_osPfsChecker",
    "osAiSetNextBuffer": "wdc_hook_osAiSetNextBuffer", "osAiGetStatus": "wdc_hook_osAiGetStatus", "osAiGetLength": "wdc_hook_osAiGetLength",
}

overlays = (GEN / "recomp_overlays.inl").read_text(encoding="utf-8")
entry_re = re.compile(r"\.func = (\w+)_recomp, \.offset = (0x[0-9A-Fa-f]+), \.rom_size = (0x[0-9A-Fa-f]+)")
ov = {m.group(1): (int(m.group(2), 16), int(m.group(3), 16)) for m in entry_re.finditer(overlays)}

defined, called = set(), set()
for f in GEN.glob("funcs_*.c"):
    text = f.read_text(encoding="utf-8")
    defined.update(re.findall(r"RECOMP_FUNC void (\w+)_recomp\(", text))
    called.update(re.findall(r"\b(\w+)_recomp\(rdram, ctx\);", text))

names = {n for n, (off, size) in ov.items() if size == 0} | (called - defined)
names -= defined

syms = {}
for line in SYMS.read_text(encoding="utf-8").splitlines():
    m = re.search(r'name = "([^"]+)", vram = (0x[0-9A-Fa-f]+)', line)
    if m:
        syms[m.group(1)] = int(m.group(2), 16)

table, unresolved = [], []
for n in sorted(names):
    if n in ov and ov[n][1] == 0:
        vram = 0x80000400 + ov[n][0]
    elif n in syms:
        vram = syms[n]
    else:
        unresolved.append(n)
        continue
    table.append((n, vram))
if unresolved:
    print("sem vram (ignorados como fronteira):", ", ".join(unresolved))

h = ["// GERADO por tools/recomp/gen_seg_hooks.py -- nao edite.",
     "// Incluido no fim de trace_hooks.h: redireciona as chamadas de funcoes do runtime para wrappers de segmento (trace_seg.cpp).",
     "#pragma once", '#include "recomp.h"', "#ifdef __cplusplus", 'extern "C" {', "#endif"]
for n, _ in table:
    h.append(f"void wdc_seg_{n}(uint8_t* rdram, recomp_context* ctx);")
h += ["#ifdef __cplusplus", "}", "#endif"]
for n, _ in table:
    h += [f"#undef {n}_recomp", f"#define {n}_recomp wdc_seg_{n}"]
OUT_H.write_text("\n".join(h) + "\n", encoding="utf-8")

c = ["// GERADO por tools/recomp/gen_seg_hooks.py -- nao edite. NAO inclui wdc_seg_hooks.h: chama as funcoes reais.",
     '#include "recomp.h"', "#include <cstddef>", 'extern "C" {',
     "void wdc_seg_call(uint8_t* rdram, recomp_context* ctx, int id, void (*real)(uint8_t*, recomp_context*));"]
for n, _ in table:
    real = HOOKED.get(n, f"{n}_recomp")
    c.append(f"void {real}(uint8_t* rdram, recomp_context* ctx);")
c.append("}")
for i, (n, _) in enumerate(table):
    real = HOOKED.get(n, f"{n}_recomp")
    c.append(f'extern "C" void wdc_seg_{n}(uint8_t* rdram, recomp_context* ctx) {{ wdc_seg_call(rdram, ctx, {i}, {real}); }}')
c.append("struct WdcSegFn { const char* name; unsigned vram; };")
c.append('extern "C" const WdcSegFn wdc_seg_table[] = {')
for n, v in table:
    c.append(f'    {{"{n}", 0x{v:08X}u}},')
c.append("};")
c.append(f'extern "C" const int wdc_seg_table_n = {len(table)};')
OUT_CPP.write_text("\n".join(c) + "\n", encoding="utf-8")
print(f"{len(table)} funcoes de runtime como fronteira de segmento -> {OUT_H.name}, {OUT_CPP.name}")
