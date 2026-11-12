"""Gera recomp/runtime_shim/recomp_shim.h: declaracoes que o runtime deve fornecer.

O N64Recomp nao declara em funcs.h as funcoes "ignoradas/renomeadas" (libultra substituida
pelo runtime), mas o C gerado as chama como <nome>_recomp(rdram, ctx). Este shim declara
cada uma delas e define MEM_WU (ausente do include/recomp.h do N64Recomp), so para permitir
validar a traducao com `clang -fsyntax-only` antes de existir um runtime de verdade.

Uso: python tools/recomp/gen_runtime_shim.py
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FUNCS = ROOT / "recomp" / "RecompiledFuncs"
OUT = ROOT / "recomp" / "runtime_shim" / "recomp_shim.h"

CALL_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\(rdram, ctx\)")
DECL_RE = re.compile(r"^void\s+([A-Za-z_][A-Za-z0-9_]*)\(uint8_t\* rdram, recomp_context\* ctx\);", re.M)

declared = set(DECL_RE.findall((FUNCS / "funcs.h").read_text(encoding="utf-8")))
called = set()
for path in FUNCS.glob("funcs_*.c"):
    called |= set(CALL_RE.findall(path.read_text(encoding="utf-8")))

missing = sorted(n for n in called - declared)
OUT.parent.mkdir(exist_ok=True)
lines = [
    "// Gerado por tools/recomp/gen_runtime_shim.py -- nao editar a mao.",
    "// Stand-in das declaracoes do runtime; so serve para validar sintaxe do C gerado.",
    '#include "recomp.h"',
    "",
    "#ifndef MEM_WU",
    "#define MEM_WU(offset, reg) \\",
    "    (*(uint32_t*)(rdram + ((((reg) + (offset))) - 0xFFFFFFFF80000000)))",
    "#endif",
    "",
    "#ifdef __cplusplus",
    'extern "C" {',
    "#endif",
    "",
]
lines += [f"void {n}(uint8_t* rdram, recomp_context* ctx);" for n in missing]
lines += ["", "#ifdef __cplusplus", "}", "#endif", ""]
OUT.write_text("\n".join(lines), encoding="utf-8")

print(f"chamadas distintas no C gerado: {len(called)}   declaradas em funcs.h: {len(called & declared)}")
print(f"fornecidas pelo runtime (declaradas no shim): {len(missing)}")
for n in missing:
    print("  ", n)
print(f"escrito: {OUT}")
