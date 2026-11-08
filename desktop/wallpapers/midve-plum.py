# Warm plum wallpaper: soft gaussian glows over a dark diagonal gradient + fine grain.
import sys, numpy as np
from PIL import Image

def hexrgb(h): return np.array([int(h[i:i+2],16) for i in (1,3,5)], dtype=np.float32)

def render(w, h, glows, out, seed=7):
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    u, v = x / w, y / h
    t = np.clip((u * 0.6 + v * 0.4), 0, 1)[..., None]
    img = hexrgb('#140c12') * (1 - t) + hexrgb('#22121b') * t
    s = max(w, h)
    for (cx, cy, r, color, a) in glows:
        d2 = ((x - cx * w) ** 2 + (y - cy * h) ** 2) / (r * s) ** 2
        k = (a * np.exp(-2.2 * d2))[..., None]
        img = img * (1 - k) + hexrgb(color) * k
    # vignette: edges sink back so the glows read as light, not as haze
    vd = ((u - 0.5) ** 2 * 1.2 + (v - 0.5) ** 2 * 1.6)[..., None]
    img *= np.clip(1.0 - vd * 0.9, 0.55, 1.0)
    rng = np.random.default_rng(seed)
    img += rng.normal(0, 2.2, img.shape).astype(np.float32)
    Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).save(out, optimize=True)

wide = [  # (x, y, radius·max-side, color, opacity)
    (0.80, 0.18, 0.40, '#9a72a6', 0.62),   # lilac, top right
    (0.16, 0.92, 0.46, '#74304a', 0.66),   # wine, bottom left
    (0.56, 0.80, 0.24, '#d89a84', 0.36),   # warm peach, low centre
    (0.34, 0.16, 0.24, '#dcc3e4', 0.14),   # pale lilac light
]
tall = [
    (0.85, 0.12, 0.40, '#9a72a6', 0.62),
    (0.10, 0.90, 0.48, '#74304a', 0.66),
    (0.60, 0.62, 0.26, '#d89a84', 0.34),
    (0.25, 0.30, 0.24, '#dcc3e4', 0.13),
]
d = sys.argv[1]
render(3440, 1440, wide, f'{d}/midve-plum-wide.png')
render(1600, 2560, tall, f'{d}/midve-plum-tall.png', seed=11)
