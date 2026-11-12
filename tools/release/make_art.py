"""Gera a arte ORIGINAL do setup e o icone (sem nenhuma imagem de terceiros): um carro esportivo estilizado, listras e uma faixa quadriculada.
Saida: recomp/project/assets/setup_hero.jpg (477x640), recomp/project/assets/wdc.ico e recomp/project/src/wdc_setup_art.h (via gen_setup_art.py).
Uso: python tools/release/make_art.py"""
import math
import random
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[2]
ASSETS = ROOT / "recomp" / "project" / "assets"
YELLOW, RED, DARK = (247, 196, 0), (214, 40, 40), (14, 16, 20)


def vgrad(w, h, top, bottom):
    g = Image.new("RGB", (1, h))
    for y in range(h):
        t = y / max(1, h - 1)
        g.putpixel((0, y), tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)))
    return g.resize((w, h))


def car_layer(size):
    """Carro esportivo de lado, virado para a direita, numa camada RGBA de `size` (largura, altura); coordenadas de projeto 1000x420."""
    W, H = 1000, 420
    s = size[0] / W
    sc = lambda pts: [(x * s, y * s) for x, y in pts]
    lay = Image.new("RGBA", (int(W * s), int(H * s)), (0, 0, 0, 0))
    # sombra no chao
    sh = Image.new("RGBA", lay.size, (0, 0, 0, 0))
    ImageDraw.Draw(sh).ellipse((40 * s, 350 * s, 960 * s, 410 * s), fill=(0, 0, 0, 170))
    lay.alpha_composite(sh.filter(ImageFilter.GaussianBlur(14 * s)))
    body_pts = [(40, 330), (36, 262), (70, 228), (170, 206), (300, 170), (430, 112), (560, 96), (690, 118), (790, 190), (905, 226), (968, 262), (972, 318), (930, 336)]
    mask = Image.new("L", lay.size, 0)
    ImageDraw.Draw(mask).polygon(sc(body_pts), fill=255)
    # cava das rodas
    for cx in (250, 735):
        ImageDraw.Draw(mask).ellipse(((cx - 92) * s, (330 - 92) * s, (cx + 92) * s, (330 + 92) * s), fill=0)
    body = vgrad(lay.size[0], lay.size[1], (255, 224, 60), (222, 150, 0)).convert("RGBA")
    d = ImageDraw.Draw(body)
    # listras de corrida (diagonais vermelhas) e linha de cintura
    for off in (0, 70):
        d.polygon(sc([(300 + off, 80), (372 + off, 80), (232 + off, 360), (160 + off, 360)]), fill=RED + (255,))
    d.polygon(sc([(40, 290), (972, 290), (972, 304), (40, 304)]), fill=(40, 26, 20, 200))
    lay.paste(body, (0, 0), mask)
    # vidros
    glass = Image.new("L", lay.size, 0)
    ImageDraw.Draw(glass).polygon(sc([(318, 176), (436, 122), (556, 106), (676, 126), (748, 188), (610, 188), (470, 188)]), fill=255)
    gl = vgrad(lay.size[0], lay.size[1], (70, 96, 130), (16, 22, 34)).convert("RGBA")
    lay.paste(gl, (0, 0), glass)
    ImageDraw.Draw(lay).line(sc([(552, 108), (552, 188)]), fill=(14, 16, 20, 255), width=max(2, int(7 * s)))
    # farol e lanterna
    d = ImageDraw.Draw(lay)
    d.polygon(sc([(930, 232), (968, 246), (966, 266), (918, 258)]), fill=(255, 250, 220, 255))
    d.polygon(sc([(40, 244), (66, 232), (64, 262), (38, 266)]), fill=(255, 70, 50, 255))
    # brilho no capo
    d.line(sc([(600, 122), (780, 188), (900, 224)]), fill=(255, 255, 255, 150), width=max(1, int(4 * s)))
    # rodas
    for cx in (250, 735):
        c = (cx * s, 330 * s)
        for r, col in ((80, (18, 18, 22)), (56, (200, 204, 214)), (44, (60, 64, 76)), (14, (230, 232, 240))):
            d.ellipse((c[0] - r * s, c[1] - r * s, c[0] + r * s, c[1] + r * s), fill=col + (255,))
        for k in range(5):
            a = k * 2 * math.pi / 5 + 0.3
            d.line((c[0], c[1], c[0] + 52 * s * math.cos(a), c[1] + 52 * s * math.sin(a)), fill=(210, 214, 224, 255), width=max(2, int(9 * s)))
    return lay


def poster():
    W, H = 954, 1280   # 2x de 477x640
    img = vgrad(W, H, (16, 18, 24), (38, 26, 36)).convert("RGBA")
    rnd = random.Random(7)
    ov = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(ov)
    for x0, col in ((-100, RED + (120,)), (60, YELLOW + (90,)), (200, RED + (70,))):   # feixes diagonais
        d.polygon([(x0 + 400, 0), (x0 + 620, 0), (x0 + 220, H), (x0 + 0, H)], fill=col)
    img.alpha_composite(ov.filter(ImageFilter.GaussianBlur(3)))
    lines = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ld = ImageDraw.Draw(lines)
    for _ in range(90):   # linhas de velocidade
        y = rnd.randint(380, 1000)
        x = rnd.randint(-80, 520)
        ld.line((x, y, x + rnd.randint(120, 420), y), fill=(255, 255, 255, rnd.randint(25, 90)), width=rnd.randint(2, 5))
    img.alpha_composite(lines)
    car = car_layer((900, 378))
    img.alpha_composite(car, (28, 470))
    # faixa quadriculada
    band = Image.new("RGBA", (W, 100), (0, 0, 0, 255))
    bd = ImageDraw.Draw(band)
    sq = 50
    for r in range(2):
        for c in range(W // sq + 1):
            if (r + c) % 2 == 0:
                bd.rectangle((c * sq, r * sq, c * sq + sq - 1, r * sq + sq - 1), fill=(240, 240, 244, 255))
    img.alpha_composite(band, (0, 900))
    d = ImageDraw.Draw(img)
    font = ImageFont.truetype("C:/Windows/Fonts/impact.ttf", 150)
    small = ImageFont.truetype("C:/Windows/Fonts/segoeuib.ttf", 40) if Path("C:/Windows/Fonts/segoeuib.ttf").exists() else ImageFont.truetype("C:/Windows/Fonts/arialbd.ttf", 40)
    d.text((60, 1020), "LEO BUILD", font=font, fill=YELLOW)
    d.text((66, 1190), "RECOMPILAÇÃO PARA PC  •  CANARY", font=small, fill=(200, 204, 214))
    # vinheta
    vg = Image.new("L", (W, H), 0)
    ImageDraw.Draw(vg).ellipse((-W * 0.3, -H * 0.1, W * 1.3, H * 1.1), fill=255)
    vg = vg.filter(ImageFilter.GaussianBlur(160))
    dark = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    img = Image.composite(img, dark, vg.point(lambda v: min(255, int(v * 1.15 + 40))))
    return img.convert("RGB").resize((477, 640), Image.LANCZOS)


def icon():
    S = 512
    bg = vgrad(S, S, (20, 22, 30), (44, 30, 40)).convert("RGBA")
    d = ImageDraw.Draw(bg)
    d.polygon([(250, 0), (400, 0), (150, S), (0, S)], fill=RED + (150,))
    d.polygon([(380, 0), (470, 0), (260, S), (170, S)], fill=YELLOW + (110,))
    for r in range(2):   # quadriculado embaixo
        for c in range(S // 32 + 1):
            if (r + c) % 2 == 0:
                d.rectangle((c * 32, 436 + r * 32, c * 32 + 31, 436 + r * 32 + 31), fill=(240, 240, 244, 255))
    bg.alpha_composite(car_layer((470, 198)), (21, 200))
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, S - 1, S - 1), radius=96, fill=255)
    out = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    out.paste(bg, (0, 0), mask)
    return out


def main():
    ASSETS.mkdir(parents=True, exist_ok=True)
    p = poster()
    tmp = ROOT / "dist" / "setup_hero_src.png"
    tmp.parent.mkdir(exist_ok=True)
    p.save(tmp)
    subprocess.check_call([sys.executable, str(ROOT / "tools" / "recomp" / "gen_setup_art.py"), str(tmp)])
    ic = icon()
    ic.save(ASSETS / "wdc.ico", sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    ic.resize((256, 256), Image.LANCZOS).save(ROOT / "dist" / "icon_preview.png")
    print("arte e icone gerados")


if __name__ == "__main__":
    main()
