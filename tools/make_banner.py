"""Generates docs/banner.png (README header and GitHub social preview, 1280x640): the app icon,
name and the desktop clock. Needs Pillow and Windows' Segoe UI fonts."""
import random
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter, ImageFont

import make_icons

ROOT = Path(__file__).resolve().parent.parent
DOCS = ROOT / "docs"
W, H = 1280, 640
S = 2  # supersample


def font(name: str, size: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(f"C:/Windows/Fonts/{name}", size * S)


def main() -> None:
    n = (W * S, H * S)
    img = Image.new("RGBA", n)
    d = ImageDraw.Draw(img)
    top, bottom = (34, 22, 84), (22, 64, 140)
    for y in range(n[1]):
        t = y / (n[1] - 1)
        d.line([(0, y), (n[0], y)], fill=tuple(round(top[i] + (bottom[i] - top[i]) * t) for i in range(3)))

    # stars: a sprinkle of dots and a few sparkles
    rnd = random.Random(7)
    for _ in range(90):
        x, y = rnd.uniform(0, n[0]), rnd.uniform(0, n[1] * 0.62)
        r = rnd.uniform(0.8, 2.0) * S
        d.ellipse((x - r, y - r, x + r, y + r), fill=(255, 255, 255, rnd.randint(70, 190)))
    for x, y, r in [(0.06, 0.12, 13), (0.47, 0.09, 9), (0.40, 0.70, 7), (0.95, 0.07, 10), (0.56, 0.22, 6)]:
        make_icons.star(d, x * n[0], y * n[1], r * S, (255, 255, 255, 230))

    def ridge(points, fill):
        layer = Image.new("RGBA", n, (0, 0, 0, 0))
        ImageDraw.Draw(layer).polygon([(x * n[0], y * n[1]) for x, y in points], fill=fill)
        img.alpha_composite(layer)

    ridge([(0, 0.80), (0.10, 0.66), (0.19, 0.74), (0.33, 0.56), (0.47, 0.72), (0.60, 0.60), (0.74, 0.74),
           (0.88, 0.58), (1, 0.70), (1, 1), (0, 1)], (255, 255, 255, 22))
    ridge([(0, 0.90), (0.08, 0.82), (0.17, 0.88), (0.30, 0.74), (0.44, 0.90), (0.56, 0.80), (0.70, 0.92),
           (0.84, 0.78), (1, 0.88), (1, 1), (0, 1)], (16, 10, 46, 255))

    # the desktop clock on the right, laid out like src/clock.cpp (Mond's sizes and spacing)
    k = 0.95 * S  # pixels per DIP
    day, date, time = "SATURDAY", "10  OCTOBER,  2026.", "- 9:41 PM -"
    fday = ImageFont.truetype(str(ROOT / "res" / "fonts" / "Audiowide.ttf"), round(40 * 4 / 3 * k))
    fsmall = ImageFont.truetype(str(ROOT / "res" / "fonts" / "Quicksand.otf"), round(14 * 4 / 3 * k))
    gap = 10 * 4 / 3 * k  # letter spacing before and after each letter
    width = sum(d.textlength(c, font=fday) + 2 * gap for c in day)
    cx, top = 930 * S, 175 * S
    glow = Image.new("RGBA", n, (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    x = cx - width / 2
    for c in day:
        gd.text((x + gap, top), c, font=fday, fill=(255, 255, 255))
        x += d.textlength(c, font=fday) + 2 * gap
    for text, y in [(date, 75), (time, 120)]:
        gd.text((cx, top + y * k), text, font=fsmall, fill=(255, 255, 255), anchor="ma")
    halo = glow.filter(ImageFilter.GaussianBlur(10 * S))
    halo.putalpha(halo.getchannel("A").point(lambda v: v * 0.55))
    img.alpha_composite(halo)
    img.alpha_composite(glow)

    # icon, name and tagline on the left
    icon = make_icons.render(256, True).resize((150 * S, 150 * S), Image.LANCZOS)
    x0 = 70 * S
    img.alpha_composite(icon, (x0 - 9 * S, 118 * S))
    d = ImageDraw.Draw(img)
    d.text((x0, 282 * S), "VideoBG", font=font("segoeuib.ttf", 76), fill=(255, 255, 255))
    d.text((x0 + 2 * S, 384 * S), "Video wallpaper and desktop clock", font=font("segoeuisl.ttf", 30),
           fill=(226, 230, 255))
    d.text((x0 + 2 * S, 424 * S), "for Windows 10 and 11", font=font("segoeuisl.ttf", 30), fill=(226, 230, 255))
    small = font("segoeui.ttf", 20)
    chips = Image.new("RGBA", n, (0, 0, 0, 0))
    cd = ImageDraw.Draw(chips)
    x = x0
    for chip in ["Hardware decoding", "Built-in clock", "Lightweight"]:
        tw = cd.textlength(chip, font=small)
        cd.rounded_rectangle((x, 486 * S, x + tw + 28 * S, 522 * S), radius=18 * S, fill=(255, 255, 255, 34),
                             outline=(255, 255, 255, 80), width=S)
        x += tw + 40 * S
    img.alpha_composite(chips)
    x = x0
    for chip in ["Hardware decoding", "Built-in clock", "Lightweight"]:
        d.text((x + 14 * S, 489 * S), chip, font=small, fill=(255, 255, 255))
        x += d.textlength(chip, font=small) + 40 * S

    img.resize((W, H), Image.LANCZOS).convert("RGB").save(DOCS / "banner.png", optimize=True)
    print("banner written to", DOCS / "banner.png")


if __name__ == "__main__":
    main()
