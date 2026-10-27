"""Sprite verticale del knob antracite (corpo neutro, senza LED) da una posa statica.

Le 14 pose fornite non sono equispaziate, quindi non sono usabili come
filmstrip. Il cappuccio centrale ha una satinatura conica (simmetrica per
rotazione): si ruota solo quello, la ghiera resta ferma.

Tutta la luce verde viene tolta (tacca accesa sostituita da una fessura spenta,
lancetta e aloni desaturati): tacche, lancetta e arco LED li disegna il
LookAndFeel nel colore del tema, quindi il knob segue il tema scelto.
Geometria per il disegno vettoriale, in frazioni del raggio dello sprite:
  tacche  r 0.62 .. 0.92, ogni 30 gradi;  lancetta r 0.24 .. 0.43.

uso: python3 make_knob_strip.py <knob_01_indicator_up.png> <out.png> [frame_px] [frames]
"""
import sys
import numpy as np
from PIL import Image

src, out = sys.argv[1], sys.argv[2]
size = int(sys.argv[3]) if len(sys.argv) > 3 else 112
frames = int(sys.argv[4]) if len(sys.argv) > 4 else 101

base = Image.open(src).convert("RGBA")
W = base.width
c = W / 2.0
CAP_R, FEATHER = 228.0, 6.0                       # cappuccio: gola scura a ~240 px su 960
RING_IN, RING_OUT, HALF = 244.0, 416.0, 13.0      # spicchio della tacca in alto

yy, xx = np.mgrid[0:W, 0:W]
r = np.hypot(xx + 0.5 - c, yy + 0.5 - c)
theta = np.degrees(np.arctan2(xx + 0.5 - c, -(yy + 0.5 - c)))   # 0 = in alto, orario
clip = lambda x: np.clip(x, 0.0, 1.0)

capMask = clip((CAP_R + FEATHER - r) / (2 * FEATHER))[..., None]
wedge = (clip((r - RING_IN) / 4.0) * clip((RING_OUT - r) / 4.0)
         * clip((HALF - np.abs(theta)) / 3.0))[..., None]

orig = np.asarray(base).astype(np.float32)
darkSlot = np.asarray(base.rotate(30.0, resample=Image.BICUBIC, center=(c, c))).astype(np.float32)
body = orig * (1 - wedge) + darkSlot * wedge

# Scala di grigi: il metallo e' gia' neutro, il verde e' solo luce dei LED.
body[..., :3] = (body[..., :3] @ np.array([0.299, 0.587, 0.114], np.float32))[..., None]

bodyImg = Image.fromarray(np.clip(body, 0, 255).astype(np.uint8), "RGBA")
strip = Image.new("RGBA", (size, size * frames))

for i in range(frames):
    pos = i / (frames - 1)
    angle = (pos - 0.5) * 270.0                                   # orario, 0 = in alto
    cap = np.asarray(bodyImg.rotate(-angle, resample=Image.BICUBIC, center=(c, c))).astype(np.float32)
    frame = body * (1 - capMask) + cap * capMask
    img = Image.fromarray(np.clip(frame, 0, 255).astype(np.uint8), "RGBA")
    img = img.crop((40, 40, W - 40, W - 40)).resize((size, size), Image.LANCZOS)
    strip.paste(img, (0, i * size))

strip.save(out, optimize=True)
print(out, strip.size)
