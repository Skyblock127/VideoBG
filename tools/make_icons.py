"""Generates res/app.ico (on) and res/app_off.ico (off) for VideoBG."""
import math
from pathlib import Path
from PIL import Image, ImageDraw

SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
RES = Path(__file__).resolve().parent.parent / "res"


def star(d: ImageDraw.ImageDraw, cx: float, cy: float, r: float, fill) -> None:
    """A four-pointed sparkle: long thin points with a pinched waist."""
    pts = []
    for i in range(8):
        a = math.pi / 4 * i - math.pi / 2
        rr = r if i % 2 == 0 else r * 0.26
        pts.append((cx + rr * math.cos(a), cy + rr * math.sin(a)))
    d.polygon(pts, fill=fill)


def render(size: int, on: bool) -> Image.Image:
    s = 8  # supersample for smooth edges
    n = size * s
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    pad = round(n * 0.06)
    box = (pad, pad, n - pad, n - pad)
    radius = round(n * 0.22)

    # night sky: vertical gradient inside a rounded square
    if on:
        top, bottom = (124, 92, 255), (36, 164, 255)
        far, near = (255, 255, 255, 46), (44, 26, 120, 235)
    else:
        top, bottom = (132, 136, 146), (92, 96, 106)
        far, near = (255, 255, 255, 40), (58, 60, 68, 235)
    grad = Image.new("RGBA", (n, n))
    gd = ImageDraw.Draw(grad)
    for y in range(n):
        t = y / (n - 1)
        c = tuple(round(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,)
        gd.line([(0, y), (n, y)], fill=c)
    mask = Image.new("L", (n, n), 0)
    ImageDraw.Draw(mask).rounded_rectangle(box, radius=radius, fill=255)
    img.paste(grad, (0, 0), mask)

    def layer(points, fill):
        shape = Image.new("L", (n, n), 0)
        ImageDraw.Draw(shape).polygon([(x * n, y * n) for x, y in points], fill=255)
        shape = Image.composite(shape, Image.new("L", (n, n), 0), mask)
        over = Image.new("RGBA", (n, n), fill[:3] + (0,))
        over.putalpha(shape.point(lambda v: v * fill[3] // 255))
        img.alpha_composite(over)

    # stars (left out where they would only be a smudge)
    if size >= 32:
        d = ImageDraw.Draw(img)
        white = (255, 255, 255, 235)
        star(d, n * 0.24, n * 0.25, n * 0.075, white)
        star(d, n * 0.80, n * 0.20, n * 0.05, white)
        star(d, n * 0.84, n * 0.44, n * 0.035, white)
        if size >= 48:
            star(d, n * 0.17, n * 0.47, n * 0.03, white)

    # mountains: a pale far range and a dark near range along the bottom
    if size >= 20:
        layer([(0, 0.72), (0.26, 0.50), (0.50, 0.70), (0.78, 0.46), (1, 0.64), (1, 1), (0, 1)], far)
    layer([(0, 0.84), (0.16, 0.72), (0.28, 0.80), (0.52, 0.58), (0.76, 0.82),
           (0.88, 0.74), (1, 0.82), (1, 1), (0, 1)], near)

    # play triangle
    d = ImageDraw.Draw(img)
    cx, cy = n * 0.52, n * 0.45
    r = n * 0.22
    tri = [(cx - r * 0.72, cy - r), (cx - r * 0.72, cy + r), (cx + r * 1.05, cy)]
    d.polygon(tri, fill=(255, 255, 255, 255 if on else 235))
    return img.resize((size, size), Image.LANCZOS)


def save(path: Path, on: bool) -> None:
    images = [render(sz, on) for sz in SIZES]
    images[-1].save(path, format="ICO", sizes=[(sz, sz) for sz in SIZES], append_images=images[:-1])


if __name__ == "__main__":
    RES.mkdir(exist_ok=True)
    save(RES / "app.ico", True)
    save(RES / "app_off.ico", False)
    print("icons written to", RES)
