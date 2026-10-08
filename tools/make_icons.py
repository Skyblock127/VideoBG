"""Generates res/app.ico (on) and res/app_off.ico (off) for VideoBG."""
from pathlib import Path
from PIL import Image, ImageDraw

SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
RES = Path(__file__).resolve().parent.parent / "res"


def render(size: int, on: bool) -> Image.Image:
    s = 8  # supersample for smooth edges
    n = size * s
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    pad = round(n * 0.06)
    box = (pad, pad, n - pad, n - pad)
    radius = round(n * 0.22)

    # background: vertical gradient inside a rounded square
    if on:
        top, bottom = (124, 92, 255), (36, 164, 255)
    else:
        top, bottom = (132, 136, 146), (92, 96, 106)
    grad = Image.new("RGBA", (n, n))
    gd = ImageDraw.Draw(grad)
    for y in range(n):
        t = y / (n - 1)
        c = tuple(round(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,)
        gd.line([(0, y), (n, y)], fill=c)
    mask = Image.new("L", (n, n), 0)
    ImageDraw.Draw(mask).rounded_rectangle(box, radius=radius, fill=255)
    img.paste(grad, (0, 0), mask)

    d = ImageDraw.Draw(img)
    # rolling "hills" silhouette along the bottom (the wallpaper)
    if size >= 24:
        hill = Image.new("L", (n, n), 0)
        hd = ImageDraw.Draw(hill)
        hd.ellipse((-n * 0.2, n * 0.66, n * 0.75, n * 1.3), fill=255)
        hd.ellipse((n * 0.35, n * 0.72, n * 1.25, n * 1.35), fill=255)
        hill = Image.composite(hill, Image.new("L", (n, n), 0), mask)
        overlay = Image.new("RGBA", (n, n), (255, 255, 255, 60))
        img.paste(overlay, (0, 0), hill)

    # play triangle
    cx, cy = n * 0.52, n * 0.47
    r = n * 0.24
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
