"""Monta o pacote da Leo Build: pasta + .zip com o executavel, o LEIA-ME e as licencas (SEM a ROM e SEM nenhum arquivo do jogo original).

Uso:  python tools/release/make_package.py [--exe caminho/wdc_recomp.exe]
Saida: dist/WorldDriverChampionship-LeoBuild-<versao>-canary-win64.zip (+ .sha256) e a pasta dist/<mesmo nome>/
A versao vem de recomp/project/src/wdc_version.h.
"""
import argparse
import hashlib
import re
import shutil
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VERSION_H = ROOT / "recomp" / "project" / "src" / "wdc_version.h"
DEFAULT_EXE = ROOT / "recomp" / "project" / "build-release" / "wdc_recomp.exe"
LICENSES = {   # (arquivo de origem, nome no pacote)
    ROOT / "thirdparty" / "N64ModernRuntime" / "COPYING": "GPL-3.0_N64ModernRuntime.txt",
    ROOT / "thirdparty" / "N64ModernRuntime" / "N64Recomp" / "LICENSE": "MIT_N64Recomp.txt",
    ROOT / "thirdparty" / "parallel-rdp-standalone" / "LICENSE": "MIT_parallel-rdp.txt",
}
THIRD_PARTY = """AVISOS DE SOFTWARE DE TERCEIROS - World Driver Championship, Leo Build

Este programa inclui:
  * N64ModernRuntime (ultramodern + librecomp) e N64Recomp - recompilacao estatica de N64 para PC.
    Licenca GNU GPL v3 (GPL-3.0_N64ModernRuntime.txt); partes sob MIT (MIT_N64Recomp.txt).
  * parallel-RDP (Themaister) - renderizador do RDP do N64 em Vulkan. Licenca MIT (MIT_parallel-rdp.txt), com volk e Vulkan-Headers.

O codigo-fonte do projeto (sem a ROM e sem o codigo recompilado derivado dela) esta no repositorio indicado no LEIA-ME/na pagina da versao.
Este e um projeto de fas, sem ligacao com a Boss Game Studios, a Midway ou a Nintendo. Nenhum arquivo do jogo original e distribuido.
"""


def read_version():
    t = VERSION_H.read_text(encoding="utf-8")
    g = lambda k: re.search(r'#define\s+%s\s+"([^"]*)"' % k, t).group(1)
    return g("WDC_BUILD_VERSION"), g("WDC_BUILD_CHANNEL")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=str(DEFAULT_EXE))
    a = ap.parse_args()
    exe = Path(a.exe)
    if not exe.exists():
        sys.exit(f"nao achei o executavel: {exe} (rode tools\\release\\build_release.cmd antes)")
    ver, chan = read_version()
    name = f"WorldDriverChampionship-LeoBuild-{ver}-{chan}-win64"
    dist = ROOT / "dist"
    out = dist / name
    if out.exists():
        shutil.rmtree(out)
    (out / "licenses").mkdir(parents=True)
    shutil.copy2(exe, out / "WorldDriverChampionship.exe")
    # LEIA-ME com quebras de linha do Windows e BOM (o Bloco de Notas mostra os acentos certos)
    readme = (ROOT / "tools" / "release" / "LEIA-ME.txt").read_text(encoding="utf-8").replace("\r\n", "\n").replace("\n", "\r\n")
    (out / "LEIA-ME.txt").write_bytes(b"\xef\xbb\xbf" + readme.encode("utf-8"))
    for src, dst in LICENSES.items():
        shutil.copy2(src, out / "licenses" / dst)
    (out / "licenses" / "AVISOS.txt").write_bytes(b"\xef\xbb\xbf" + THIRD_PARTY.replace("\n", "\r\n").encode("utf-8"))
    zpath = dist / (name + ".zip")
    if zpath.exists():
        zpath.unlink()
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for f in sorted(out.rglob("*")):
            if f.is_file():
                z.write(f, Path(name) / f.relative_to(out))
    h = hashlib.sha256(zpath.read_bytes()).hexdigest()
    (dist / (name + ".zip.sha256")).write_text(f"{h}  {zpath.name}\n", encoding="utf-8")
    print(f"pacote: {zpath} ({zpath.stat().st_size / 1e6:.1f} MB)\nsha256: {h}")


if __name__ == "__main__":
    main()
