"""Texture del tema "Acid Neon": metallo nero opaco (micro-grana, niente lucido) + banda LED verde neon.

Procedurale (rumore spazzolato orizzontale) e deterministico: rigenerabile.
Le immagini vengono affiancate a specchio da TexturedPanel, quindi non serve
che siano periodiche.

uso: python3 make_theme.py <cartella Assets>
"""
import sys
import numpy as np
from PIL import Image, ImageFilter

out = sys.argv[1]
rng = np.random.default_rng(303)


def brushed(w, h, base, contrast, grain, sheenAmt=0.0):
    # Satinatura: rumore fortemente sfocato lungo x, quasi nitido lungo y.
    n = rng.normal(0.0, 1.0, (h, w * 3)).astype(np.float32)
    k = np.ones(61, np.float32) / 61
    n = np.apply_along_axis(lambda r: np.convolve(r, k, mode="same"), 1, n)[:, w:2 * w]
    n /= n.std() + 1e-6
    fine = rng.normal(0.0, 1.0, (h, w)).astype(np.float32)
    v = n * contrast + fine * grain
    # Riflesso morbido diagonale: il satinato non è mai piatto.
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    sheen = np.exp(-((xx / w - 0.35) + (yy / h - 0.5) * 0.6) ** 2 / 0.08) * sheenAmt
    rgb = np.stack([base[0] + v + sheen, base[1] + v + sheen, base[2] + v * 1.05 + sheen * 1.1], -1)
    return np.clip(rgb, 0, 255)


def save(arr, name):
    a = np.dstack([arr, np.full(arr.shape[:2], 255.0)]).astype(np.uint8)
    Image.fromarray(a, "RGBA").save(f"{out}/{name}", optimize=True)
    print(name, a.shape[1], a.shape[0])


ACID = np.array([51, 255, 106], np.float32)   # verde neon


def grid(arr, step, amt):
    # Reticolo di micro-punti: richiamo "tech" appena percepibile.
    h, w = arr.shape[:2]
    yy, xx = np.mgrid[0:h, 0:w]
    dots = ((yy % step) == 0) & ((xx % step) == 0)
    arr[dots] += amt
    return arr


save(brushed(700, 260, (12, 14, 17), 1.3, 0.9), "panel_metal.png")               # testata
save(grid(brushed(700, 340, (6, 7, 9), 0.8, 0.6), 16, 12.0), "panel_dark.png")   # corpo
mod = brushed(320, 200, (19, 20, 22), 1.8, 1.2)
yy, xx = np.mgrid[0:200, 0:320].astype(np.float32)
vign = 1.0 - 0.10 * (((xx / 320 - 0.5) * 2) ** 2 + ((yy / 200 - 0.5) * 2) ** 2) / 2
save(mod * vign[..., None], "panel_module.png")                       # moduli

# Banda: canale scuro con tubo LED verde al centro e alone.
band = brushed(700, 24, (14, 15, 17), 2.0, 1.0)
y = np.arange(24, dtype=np.float32)[:, None]
core = np.exp(-((y - 11.5) ** 2) / 1.2)
halo = np.exp(-((y - 11.5) ** 2) / 18.0)
glow = ACID
band = band + core[..., None] * glow * 0.95 + halo[..., None] * glow * 0.35
save(np.clip(band, 0, 255), "band_orange.png")
