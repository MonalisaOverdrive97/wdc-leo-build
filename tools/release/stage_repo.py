"""Monta dist/repo/ com o que vai para o GitHub: o codigo-fonte do PROJETO (GPL-3.0, por causa do runtime), sem a ROM e sem nada derivado dela
(C recompilado, ucode recompilado, disassemblies, listas geradas). Uso: python tools/release/stage_repo.py"""
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "dist" / "repo_stage"

INCLUDE_DIRS = {   # origem -> sufixos permitidos
    "recomp/project/src": {".cpp", ".h", ".hpp", ".rc"},
    "recomp/project/assets": {".jpg", ".ico"},
    "recomp/project/test": {".cpp", ".cmd"},
    "recomp/runtime_glue": {".cpp"},
    "recomp/runtime_shim": {".h"},
    "tools/release": {".cmd", ".py", ".txt"},
}
INCLUDE_FILES = [
    "recomp/project/CMakeLists.txt",
    "recomp/ENHANCED_MODE.md",
    "recomp/TESTES.md",
    "tools/recomp/build_project.cmd",
    "tools/recomp/build_runtime.cmd",
    "tools/recomp/gen_wdc_toml.py",
    "tools/recomp/gen_runtime_shim.py",
    "tools/recomp/gen_seg_hooks.py",
    "tools/recomp/gen_setup_art.py",
    "tools/recomp/libultra_names.json",
]
# jamais: ROM, C/ucode gerados, disassemblies (derivados do jogo)
FORBIDDEN_PARTS = {"rom", "RecompiledFuncs", "RecompiledFuncs60", "RecompiledFuncsTrace", "RecompiledUcode", "asm", "build", "build-release", "build-trace", "dist", "explore"}
FORBIDDEN_SUFFIX = {".z64", ".n64", ".v64", ".bin", ".exe", ".obj", ".mpk"}


def ok(p: Path) -> bool:
    rel = p.relative_to(ROOT)
    return not (set(rel.parts) & FORBIDDEN_PARTS) and p.suffix.lower() not in FORBIDDEN_SUFFIX


def main():
    def _force(func, path, exc):   # arquivos do .git sao somente leitura no Windows
        import os, stat
        os.chmod(path, stat.S_IWRITE)
        func(path)
    if OUT.exists():
        shutil.rmtree(OUT, onexc=_force)
    OUT.mkdir(parents=True)
    n = 0
    def copy(src: Path):
        nonlocal n
        if not ok(src):
            sys.exit(f"proibido: {src}")
        dst = OUT / src.relative_to(ROOT)
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        n += 1
    for d, sufs in INCLUDE_DIRS.items():
        for f in sorted((ROOT / d).iterdir()):
            if f.is_file() and f.suffix.lower() in sufs:
                copy(f)
    for f in INCLUDE_FILES:
        copy(ROOT / f)
    # README, LICENSE (GPL-3.0 do runtime), patch do runtime, .gitignore
    shutil.copy2(ROOT / "tools" / "release" / "README_repo.md", OUT / "README.md")
    shutil.copy2(ROOT / "thirdparty" / "N64ModernRuntime" / "COPYING", OUT / "LICENSE")
    (OUT / "patches").mkdir()
    shutil.copy2(ROOT / "tools" / "release" / "patches" / "N64ModernRuntime-reserve-not-commit.patch", OUT / "patches" / "N64ModernRuntime-reserve-not-commit.patch")
    (OUT / ".gitignore").write_text("build*/\ndist/\n*.exe\n*.obj\n*.z64\n*.v64\n*.n64\n*.mpk\nwdc_config/\nRecompiledFuncs*/\nrom/\n", encoding="utf-8")
    print(f"{n + 4} arquivos em {OUT}")


if __name__ == "__main__":
    main()
