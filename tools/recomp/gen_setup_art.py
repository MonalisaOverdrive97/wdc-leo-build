"""Converte a imagem do setup (png/webp/jpg) para JPEG (qualidade 93) e gera recomp/project/src/wdc_setup_art.h (bytes do JPEG embutidos no executavel; o GDI+ decodifica).
Uso: python tools/recomp/gen_setup_art.py <imagem> [largura_max]   (a arte convertida fica em recomp/project/assets/setup_hero.jpg)"""
import io
import sys
from pathlib import Path

from PIL import Image

root = Path(__file__).resolve().parents[2]
src = Path(sys.argv[1])
max_w = int(sys.argv[2]) if len(sys.argv) > 2 else 477
im = Image.open(src).convert("RGB")
if im.size[0] > max_w:
    im = im.resize((max_w, round(im.size[1] * max_w / im.size[0])), Image.LANCZOS)
out_png = root / "recomp" / "project" / "assets" / "setup_hero.jpg"
out_png.parent.mkdir(parents=True, exist_ok=True)
im.save(out_png, quality=93, subsampling=0, optimize=True)
data = out_png.read_bytes()
lines = ["// Gerado por tools/recomp/gen_setup_art.py a partir de recomp/project/assets/setup_hero.jpg (%dx%d, %d bytes). Nao editar." % (im.size[0], im.size[1], len(data)),
         "#pragma once", "#include <cstddef>", "static const unsigned kSetupArtW = %d, kSetupArtH = %d;" % im.size,
         "static const unsigned char kSetupArt[] = {"]
for i in range(0, len(data), 24):
    lines.append("    " + ",".join(str(b) for b in data[i:i + 24]) + ",")
lines += ["};", "static const size_t kSetupArtSize = sizeof(kSetupArt);"]
(root / "recomp" / "project" / "src" / "wdc_setup_art.h").write_text("\n".join(lines) + "\n", encoding="utf-8")
print("arte:", im.size, "JPEG", len(data), "bytes")
