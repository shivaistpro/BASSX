#!/usr/bin/env python3
"""
BASSX — generatore di texture fotorealistiche.

Perché generate e non fotografate:
    Le fotografie di pannelli di sintetizzatori commerciali sono materiale
    protetto (copyright sulla foto, marchio sulla grafica del pannello).
    Questo script produce texture *fisicamente plausibili* — non pattern
    geometrici — modellando i fenomeni ottici reali:

      * alluminio spazzolato: riflessione anisotropa, cioè un highlight che si
        allunga perpendicolarmente alla direzione della spazzolatura. È questo,
        e non il colore, a far leggere una superficie come "metallo lavorato".
      * plastica ABS: microrugosità (grana) + riflessione speculare larga e
        debole + component diffusa scura.
      * cromo: riflessione quasi totale di un ambiente. Un cromo convincente è
        interamente definito dall'ambiente che riflette; qui l'ambiente è una
        mappa sintetica con cielo chiaro in alto, orizzonte netto e pavimento
        scuro, più due sorgenti luminose. È la ragione per cui il knob mostra
        la caratteristica banda scura orizzontale a metà altezza.
      * occlusione ambientale nei recessi e ombre proiettate coerenti con una
        singola luce a 55° in alto a sinistra.

Uscita: PNG a 32 bit in ../Assets, poi incorporati nel binario via JUCE
BinaryData.

Uso:  python3 generate_textures.py [cartella_di_uscita]
"""

import os
import sys
import math
import numpy as np
from PIL import Image, ImageFilter

# =============================================================================
# Impostazioni globali
# =============================================================================

# Fattore di sovracampionamento: tutto viene renderizzato a SS volte la
# risoluzione finale e poi ridotto con filtro di Lanczos. Serve a ottenere bordi
# antialiasati senza dover implementare il coverage analitico per ogni forma.
SS = 3

# Direzione della luce principale, coerente su tutte le texture.
# Convenzione: y cresce verso il basso, quindi una luce "in alto a sinistra" ha
# componente y negativa.
LIGHT = np.array([-0.55, -0.66, 0.51])
LIGHT /= np.linalg.norm(LIGHT)

# Seconda luce, fredda, da destra: riempie le ombre ed è ciò che impedisce alle
# superfici curve di apparire "piatte e nere" sul lato in ombra.
FILL = np.array([0.62, -0.30, 0.72])
FILL /= np.linalg.norm(FILL)

RNG = np.random.default_rng(20260813)


# =============================================================================
# Utility
# =============================================================================

def save(img, path):
    img.save(path, "PNG", optimize=True)
    print(f"  {os.path.basename(path):28s} {img.size[0]}x{img.size[1]}")


def downsample(arr, factor=SS):
    """arr: HxWx4 float 0..1 -> Image RGBA ridotta."""
    h, w = arr.shape[:2]
    img = Image.fromarray(np.clip(arr * 255.0, 0, 255).astype(np.uint8), "RGBA")
    return img.resize((w // factor, h // factor), Image.LANCZOS)


def value_noise(shape, scale, octaves=4, persistence=0.5, rng=None):
    """Rumore frazionario per interpolazione bilineare di griglie casuali.

    Non è Perlin: è value noise. Per una grana di superficie la differenza è
    invisibile e questo costa un decimo.
    """
    rng = rng or RNG
    h, w = shape
    total = np.zeros((h, w), dtype=np.float64)
    amplitude = 1.0
    norm = 0.0

    for o in range(octaves):
        gh = max(2, int(h / (scale / (2 ** o))))
        gw = max(2, int(w / (scale / (2 ** o))))
        grid = rng.random((gh, gw))
        layer = np.array(Image.fromarray((grid * 255).astype(np.uint8)).resize((w, h), Image.BICUBIC)) / 255.0
        total += layer * amplitude
        norm += amplitude
        amplitude *= persistence

    return total / norm


def normalize_map(nx, ny, nz):
    length = np.sqrt(nx * nx + ny * ny + nz * nz)
    length = np.maximum(length, 1e-9)
    return nx / length, ny / length, nz / length


def environment(nx, ny, nz):
    """Mappa d'ambiente sintetica per superfici speculari.

    Restituisce la radianza riflessa (RGB) per una normale data, assumendo
    vista lungo +z. L'ambiente è: cielo luminoso in alto, orizzonte netto,
    pavimento scuro, più due sorgenti puntiformi.

    L'orizzonte netto è il dettaglio che fa la differenza. Un cromo con un
    gradiente morbido dall'alto al basso sembra plastica lucida; è la
    discontinuità netta a metà altezza a farlo leggere come metallo.
    """
    # Vettore riflesso per vista lungo l'asse z: r = 2(n·v)n - v, con v = (0,0,1)
    rx = 2.0 * nz * nx
    ry = 2.0 * nz * ny
    rz = 2.0 * nz * nz - 1.0

    elevation = -ry  # +1 = verso l'alto

    sky = np.clip((elevation - 0.02) * 6.0, 0.0, 1.0)
    floor = np.clip((-elevation - 0.05) * 3.0, 0.0, 1.0)

    # Gradiente del cielo: più luminoso verso lo zenit.
    sky_lum = 0.72 + 0.28 * np.clip(elevation, 0, 1)

    r = sky * sky_lum * 0.98 + floor * 0.10 + (1 - sky - floor) * 0.34
    g = sky * sky_lum * 1.00 + floor * 0.10 + (1 - sky - floor) * 0.34
    b = sky * sky_lum * 1.04 + floor * 0.12 + (1 - sky - floor) * 0.36

    # Sorgente principale: lobo speculare stretto.
    spec_main = np.clip(rx * LIGHT[0] + ry * LIGHT[1] + rz * LIGHT[2], 0, 1) ** 48
    # Sorgente di riempimento: lobo largo.
    spec_fill = np.clip(rx * FILL[0] + ry * FILL[1] + rz * FILL[2], 0, 1) ** 12

    r += spec_main * 1.5 + spec_fill * 0.22
    g += spec_main * 1.5 + spec_fill * 0.24
    b += spec_main * 1.45 + spec_fill * 0.28

    # Striscia di riflessione orizzontale: la finestra della stanza. Presente in
    # quasi ogni fotografia di hardware ed è ciò che rende "credibile" il metallo.
    window = np.exp(-((elevation - 0.42) ** 2) / 0.004) * np.clip(1.0 - abs(rx) * 0.6, 0, 1)
    r += window * 0.55
    g += window * 0.57
    b += window * 0.60

    return r, g, b


# =============================================================================
# Alluminio spazzolato
# =============================================================================

def brushed_aluminium(w, h, base=0.70, streak_strength=0.030, tint=(1.0, 1.0, 1.02)):
    """Pannello in alluminio spazzolato orizzontalmente.

    La spazzolatura è modellata come perturbazione della normale lungo il solo
    asse verticale: i solchi corrono in orizzontale, quindi la normale varia in
    y e resta costante in x. Il risultato è la riflessione anisotropa —
    highlight allungati in orizzontale — che è la firma visiva di questo
    materiale.
    """
    ww, hh = w * SS, h * SS

    # Solchi: rumore ad altissima frequenza in verticale, correlato in
    # orizzontale su lunghe distanze (è così che li lascia una spazzola).
    # La spazzola lascia solchi lunghissimi: la correlazione orizzontale deve
    # essere di centinaia di pixel, quella verticale di uno solo. Generare il
    # rumore a bassa risoluzione orizzontale e stirarlo è esattamente questo.
    fine = RNG.normal(0.0, 1.0, (hh, max(6, ww // 160)))
    fine = np.array(Image.fromarray(((fine * 0.16 + 0.5) * 255).clip(0, 255).astype(np.uint8))
                    .resize((ww, hh), Image.BICUBIC)) / 255.0 - 0.5

    # Secondo strato, solchi più corti: senza, la superficie sembra spazzolata
    # da una macchina perfetta invece che da un utensile reale.
    mid = RNG.normal(0.0, 1.0, (hh, max(8, ww // 26)))
    mid = np.array(Image.fromarray(((mid * 0.16 + 0.5) * 255).clip(0, 255).astype(np.uint8))
                   .resize((ww, hh), Image.BICUBIC)) / 255.0 - 0.5

    micro = RNG.normal(0.0, 1.0, (hh, ww)) * 0.012

    grooves = fine * 2.2 + mid * 0.9 + micro

    # Derivata verticale dei solchi -> componente y della normale. Il fattore
    # verticale è dieci volte quello orizzontale: è questa asimmetria a produrre
    # la riflessione anisotropa.
    dy = np.gradient(grooves, axis=0) * 15.0
    dx = np.gradient(grooves, axis=1) * 1.2

    nx, ny, nz = normalize_map(dx, dy, np.ones_like(dy))

    er, eg, eb = environment(nx, ny, nz)

    # L'alluminio spazzolato è per metà speculare e per metà diffuso.
    diffuse = np.clip(nx * LIGHT[0] + ny * LIGHT[1] + nz * LIGHT[2], 0, 1)

    lum = base * (0.62 + 0.38 * diffuse)

    r = lum + er * 0.22 + grooves * streak_strength
    g = lum + eg * 0.22 + grooves * streak_strength
    b = lum + eb * 0.22 + grooves * streak_strength

    # Gradiente verticale: il pannello riceve più luce in alto.
    grad = np.linspace(1.06, 0.90, hh)[:, None]
    r *= grad; g *= grad; b *= grad

    # Vignettatura leggera agli angoli.
    yy, xx = np.mgrid[0:hh, 0:ww]
    cx, cy = ww / 2.0, hh / 2.0
    d = np.sqrt(((xx - cx) / cx) ** 2 + ((yy - cy) / cy) ** 2)
    vig = 1.0 - 0.12 * np.clip(d - 0.55, 0, 2) ** 2
    r *= vig; g *= vig; b *= vig

    out = np.zeros((hh, ww, 4))
    out[..., 0] = np.clip(r * tint[0], 0, 1)
    out[..., 1] = np.clip(g * tint[1], 0, 1)
    out[..., 2] = np.clip(b * tint[2], 0, 1)
    out[..., 3] = 1.0

    return downsample(out)


# =============================================================================
# Plastica ABS
# =============================================================================

def abs_plastic(w, h, colour=(0.085, 0.088, 0.095), grain=0.030, sheen=0.16):
    """Plastica ABS stampata: grana fine isotropa, riflessione speculare larga.

    A differenza del metallo, la componente speculare della plastica è
    acromatica (colore della luce, non del materiale) e larga. Sommare una
    speculare colorata è l'errore che fa sembrare la plastica gomma verniciata.
    """
    ww, hh = w * SS, h * SS

    bump = value_noise((hh, ww), scale=max(6, ww // 220), octaves=4) - 0.5
    bump += RNG.normal(0, 1, (hh, ww)) * 0.30

    dx = np.gradient(bump, axis=1) * 9.0
    dy = np.gradient(bump, axis=0) * 9.0
    nx, ny, nz = normalize_map(dx, dy, np.ones_like(dx))

    diffuse = np.clip(nx * LIGHT[0] + ny * LIGHT[1] + nz * LIGHT[2], 0, 1)
    fill = np.clip(nx * FILL[0] + ny * FILL[1] + nz * FILL[2], 0, 1)

    # Speculare di Blinn-Phong con esponente basso: superficie satinata.
    view = np.array([0.0, 0.0, 1.0])
    hvec = LIGHT + view
    hvec /= np.linalg.norm(hvec)
    spec = np.clip(nx * hvec[0] + ny * hvec[1] + nz * hvec[2], 0, 1) ** 22

    shade = 0.35 + 0.55 * diffuse + 0.22 * fill

    out = np.zeros((hh, ww, 4))
    for c in range(3):
        out[..., c] = np.clip(colour[c] * shade + spec * sheen + bump * grain, 0, 1)

    grad = np.linspace(1.14, 0.88, hh)[:, None]
    out[..., :3] *= grad[..., None]

    out[..., 3] = 1.0
    return downsample(out)


# =============================================================================
# Knob cromato — sprite sheet
# =============================================================================

def render_knob(size, angle_deg, style="chrome"):
    """Renderizza un knob con la lancetta a `angle_deg` (0 = ore 12).

    Geometria modellata:
        r < 0.62   calotta superiore leggermente bombata, satinata
        0.62..0.72 smusso
        0.72..0.94 gonna zigrinata (knurling), cilindrica
        0.94..1.00 base con occlusione ambientale
    Sotto, ombra proiettata.
    """
    n = size * SS
    yy, xx = np.mgrid[0:n, 0:n]
    cx = cy = (n - 1) / 2.0

    # Coordinate normalizzate; il knob occupa l'86% del riquadro, il resto è
    # margine per l'ombra proiettata.
    x = (xx - cx) / (n * 0.43)
    y = (yy - cy) / (n * 0.43)
    r = np.sqrt(x * x + y * y)
    theta = np.arctan2(y, x)

    inside = r <= 1.0

    # --- Normali della superficie -------------------------------------------
    nz = np.ones_like(r)
    nx = np.zeros_like(r)
    ny = np.zeros_like(r)

    # Calotta: bombatura dolce (sfera molto piatta).
    cap = r < 0.62
    dome = np.clip(r / 0.62, 0, 1)
    # Curvatura marcata: la calotta deve spazzare abbastanza ambiente da
    # mostrare sia il cielo sia l'orizzonte, altrimenti il cromo si legge come
    # plastica bianca lucida.
    nx = np.where(cap, x * 1.05 * dome, nx)
    ny = np.where(cap, y * 1.05 * dome, ny)

    # Smusso: transizione ripida verso l'esterno.
    bevel = (r >= 0.62) & (r < 0.72)
    t = np.clip((r - 0.62) / 0.10, 0, 1)
    with np.errstate(invalid="ignore", divide="ignore"):
        ux = np.where(r > 1e-6, x / np.maximum(r, 1e-6), 0.0)
        uy = np.where(r > 1e-6, y / np.maximum(r, 1e-6), 0.0)
    nx = np.where(bevel, ux * (0.26 + 1.45 * t), nx)
    ny = np.where(bevel, uy * (0.26 + 1.45 * t), ny)
    nz = np.where(bevel, 1.0 - 0.55 * t, nz)

    # Gonna cilindrica con zigrinatura.
    skirt = (r >= 0.72) & (r <= 1.0)
    grooves = 44
    knurl = np.sin(theta * grooves + math.radians(angle_deg) * grooves / 6.0)
    knurl_n = knurl * 0.62

    # Su un cilindro visto di fronte, la normale ruota verso l'osservatore man
    # mano che ci si allontana dal bordo.
    edge_t = np.clip((r - 0.72) / 0.28, 0, 1)
    nx = np.where(skirt, ux * (0.55 + 0.45 * edge_t) - uy * knurl_n * 0.55, nx)
    ny = np.where(skirt, uy * (0.55 + 0.45 * edge_t) + ux * knurl_n * 0.55, ny)
    nz = np.where(skirt, np.maximum(0.12, 1.0 - edge_t * 0.95), nz)

    nx, ny, nz = normalize_map(nx, ny, nz)

    # --- Materiale -----------------------------------------------------------
    er, eg, eb = environment(nx, ny, nz)
    diffuse = np.clip(nx * LIGHT[0] + ny * LIGHT[1] + nz * LIGHT[2], 0, 1)
    fill = np.clip(nx * FILL[0] + ny * FILL[1] + nz * FILL[2], 0, 1)

    # Il cromo è quasi puramente speculare: la componente diffusa di un metallo
    # lucidato è sotto il 5%. Darle peso è l'errore che produce il classico
    # "knob di plastica bianca" delle interfacce disegnate a mano.
    if style == "chrome":
        base_r, base_g, base_b = 0.30, 0.305, 0.325
        spec_mix = 1.05
        diffuse_weight = 0.18
    elif style == "dark":
        base_r, base_g, base_b = 0.105, 0.108, 0.116
        spec_mix = 0.30
        diffuse_weight = 0.62
    else:  # "orange" — pomello colorato
        base_r, base_g, base_b = 0.66, 0.27, 0.05
        spec_mix = 0.34
        diffuse_weight = 0.58

    shade = 0.30 + diffuse_weight * diffuse + 0.26 * fill

    cr = base_r * shade + er * spec_mix
    cg = base_g * shade + eg * spec_mix
    cb = base_b * shade + eb * spec_mix

    # Spazzolatura circolare sulla calotta: solchi concentrici.
    circ = (np.sin(r * 260.0) * 0.5 + np.sin(r * 91.0) * 0.5) * 0.022
    circ = np.where(cap, circ, 0.0)
    cr += circ; cg += circ; cb += circ

    # Ombreggiatura della zigrinatura: i solchi sono in ombra.
    knurl_shade = np.where(skirt, np.clip(knurl, -1, 1) * 0.13, 0.0)
    cr += knurl_shade; cg += knurl_shade; cb += knurl_shade

    # Occlusione ambientale al piede del knob.
    ao = np.where(skirt, 1.0 - 0.45 * edge_t ** 2, 1.0)
    cr *= ao; cg *= ao; cb *= ao

    # --- Indicatore ----------------------------------------------------------
    # Solco inciso, non una linea disegnata: ha un lato in ombra e uno in luce,
    # come qualunque incisione reale.
    a = math.radians(angle_deg - 90.0)
    dirx, diry = math.cos(a), math.sin(a)
    # Distanza dal segmento che va da r=0.15 a r=0.92 lungo la direzione.
    proj = x * dirx + y * diry
    perp = -x * diry + y * dirx
    on_line = (proj > 0.16) & (proj < 0.93) & (np.abs(perp) < 0.052)

    line_shape = np.clip(1.0 - (np.abs(perp) / 0.052) ** 2, 0, 1)

    # Il solco è scuro al centro, con un labbro illuminato sul lato rivolto
    # verso la luce. Un indice disegnato come linea bianca piatta sparisce
    # sulla calotta cromata, che è già bianca: il contrasto deve venire dalla
    # geometria, non dal colore.
    lip = np.clip(perp / 0.052, -1, 1)
    lit_lip = np.clip(lip * (dirx * LIGHT[0] + diry * LIGHT[1]) * -1.0, 0, 1) ** 2

    groove_dark = 0.09 if style == "chrome" else 0.92
    cr = np.where(on_line, cr * (1.0 - 0.92 * line_shape) + line_shape * groove_dark + lit_lip * 0.85, cr)
    cg = np.where(on_line, cg * (1.0 - 0.92 * line_shape) + line_shape * groove_dark + lit_lip * 0.85, cg)
    cb = np.where(on_line, cb * (1.0 - 0.92 * line_shape) + line_shape * (groove_dark + 0.01) + lit_lip * 0.87, cb)

    # --- Composizione con alfa e ombra proiettata ----------------------------
    out = np.zeros((n, n, 4))

    # Ombra: copia sfocata del disco, spostata secondo la luce.
    shadow_alpha = np.zeros((n, n))
    sx = int(n * 0.030)
    sy = int(n * 0.042)
    disc = (r <= 1.0).astype(np.float64)
    shadow_alpha[max(0, sy):, max(0, sx):] = disc[:n - sy if sy else n, :n - sx if sx else n]
    shadow_img = Image.fromarray((shadow_alpha * 255).astype(np.uint8)).filter(
        ImageFilter.GaussianBlur(radius=n * 0.035))
    shadow_alpha = np.array(shadow_img) / 255.0 * 0.55

    # Antialiasing del bordo del knob tramite copertura analitica approssimata.
    edge_px = 1.0 / (n * 0.43)
    coverage = np.clip((1.0 - r) / edge_px, 0, 1)

    alpha = np.maximum(shadow_alpha * (1.0 - coverage), coverage)

    # Colore: dove c'è solo ombra il colore è nero.
    for c, ch in enumerate((cr, cg, cb)):
        col = np.clip(ch, 0, 1) * coverage
        out[..., c] = np.where(alpha > 1e-6, col / np.maximum(alpha, 1e-6) * np.maximum(coverage, 0.0)
                               + 0.0, 0.0)

    # Semplificazione: premoltiplicazione evitata, si compone il colore del
    # knob dove coperto e nero dove è solo ombra.
    for c, ch in enumerate((cr, cg, cb)):
        out[..., c] = np.clip(ch, 0, 1) * coverage

    out[..., 3] = np.clip(alpha, 0, 1)

    return out


def knob_sheet(size, frames=65, style="chrome", sweep=270.0):
    """Sprite sheet verticale: `frames` fotogrammi impilati."""
    sheet = Image.new("RGBA", (size, size * frames), (0, 0, 0, 0))

    for i in range(frames):
        angle = -sweep / 2.0 + sweep * i / (frames - 1)
        arr = render_knob(size, angle, style)
        img = downsample(arr)
        sheet.paste(img, (0, i * size))

    return sheet


# =============================================================================
# Componenti minori
# =============================================================================

def screw(size=32):
    n = size * SS
    yy, xx = np.mgrid[0:n, 0:n]
    c = (n - 1) / 2.0
    x = (xx - c) / (n * 0.42)
    y = (yy - c) / (n * 0.42)
    r = np.sqrt(x * x + y * y)

    # Testa svasata: conica verso il centro.
    nx = x * 0.75
    ny = y * 0.75
    nz = np.ones_like(r) * 1.0
    nx, ny, nz = normalize_map(nx, ny, nz)

    er, eg, eb = environment(nx, ny, nz)
    diffuse = np.clip(nx * LIGHT[0] + ny * LIGHT[1] + nz * LIGHT[2], 0, 1)

    base = 0.46 + 0.34 * diffuse
    cr = base + er * 0.42
    cg = base + eg * 0.42
    cb = base + eb * 0.44

    # Intaglio a croce (Phillips), inciso.
    cross = ((np.abs(x) < 0.085) | (np.abs(y) < 0.085)) & (r < 0.72)
    depth = np.clip(1.0 - np.minimum(np.abs(x), np.abs(y)) / 0.085, 0, 1)
    cr = np.where(cross, cr * (0.30 + 0.20 * (1 - depth)), cr)
    cg = np.where(cross, cg * (0.30 + 0.20 * (1 - depth)), cg)
    cb = np.where(cross, cb * (0.32 + 0.20 * (1 - depth)), cb)

    edge_px = 1.0 / (n * 0.42)
    coverage = np.clip((1.0 - r) / edge_px, 0, 1)

    # Anello di occlusione: la vite è avvitata dentro il pannello.
    ring = np.clip((r - 0.86) / 0.14, 0, 1)
    cr *= 1.0 - 0.55 * ring
    cg *= 1.0 - 0.55 * ring
    cb *= 1.0 - 0.55 * ring

    out = np.zeros((n, n, 4))
    out[..., 0] = np.clip(cr, 0, 1) * coverage
    out[..., 1] = np.clip(cg, 0, 1) * coverage
    out[..., 2] = np.clip(cb, 0, 1) * coverage
    out[..., 3] = coverage

    return downsample(out)


def led(size=28, colour=(1.0, 0.20, 0.10), on=True):
    """LED a cupola. Da spento resta una lente scura semitrasparente: un LED
    spento disegnato come cerchio grigio piatto è il dettaglio che fa sembrare
    finta un'interfaccia."""
    n = size * SS
    yy, xx = np.mgrid[0:n, 0:n]
    c = (n - 1) / 2.0
    x = (xx - c) / (n * 0.34)
    y = (yy - c) / (n * 0.34)
    r = np.sqrt(x * x + y * y)

    nz = np.sqrt(np.clip(1.0 - r * r, 0, 1))
    nx, ny, nzz = normalize_map(x, y, np.maximum(nz, 0.08))

    spec = np.clip(nx * LIGHT[0] + ny * LIGHT[1] + nzz * LIGHT[2], 0, 1) ** 30

    if on:
        # Emissione: massima al centro, con Fresnel che scurisce il bordo.
        core = np.clip(1.0 - r ** 1.7, 0, 1)
        emis = core * 1.35 + 0.25
        cr = colour[0] * emis
        cg = colour[1] * emis
        cb = colour[2] * emis
    else:
        body = 0.16 + 0.10 * nz
        cr = colour[0] * 0.20 * body
        cg = colour[1] * 0.20 * body
        cb = colour[2] * 0.20 * body

    cr += spec * 0.95
    cg += spec * 0.95
    cb += spec * 0.95

    edge_px = 1.0 / (n * 0.34)
    coverage = np.clip((1.0 - r) / edge_px, 0, 1)

    out = np.zeros((n, n, 4))
    out[..., 0] = np.clip(cr, 0, 1) * coverage
    out[..., 1] = np.clip(cg, 0, 1) * coverage
    out[..., 2] = np.clip(cb, 0, 1) * coverage
    out[..., 3] = coverage

    img = downsample(out)

    if on:
        # Alone luminoso: composito additivo attorno al LED, come il bloom di
        # una sorgente puntiforme in una fotografia.
        glow = Image.new("RGBA", img.size, (0, 0, 0, 0))
        gd = np.zeros((size, size, 4))
        gy, gx = np.mgrid[0:size, 0:size]
        gc = (size - 1) / 2.0
        gr = np.sqrt(((gx - gc) / (size * 0.5)) ** 2 + ((gy - gc) / (size * 0.5)) ** 2)
        halo = np.clip(1.0 - gr, 0, 1) ** 2.2
        gd[..., 0] = colour[0]
        gd[..., 1] = colour[1]
        gd[..., 2] = colour[2]
        gd[..., 3] = halo * 0.55
        glow = Image.fromarray((np.clip(gd, 0, 1) * 255).astype(np.uint8), "RGBA")
        img = Image.alpha_composite(glow, img)

    return img


def button(w=64, h=34, pressed=False, tint=(0.13, 0.135, 0.145)):
    """Tasto in gomma siliconica con bordo arrotondato."""
    ww, hh = w * SS, h * SS
    yy, xx = np.mgrid[0:hh, 0:ww]

    radius = min(ww, hh) * 0.28

    # Campo di distanza da un rettangolo con angoli arrotondati.
    dx = np.maximum(np.abs(xx - (ww - 1) / 2.0) - (ww / 2.0 - radius), 0)
    dy = np.maximum(np.abs(yy - (hh - 1) / 2.0) - (hh / 2.0 - radius), 0)
    dist = np.sqrt(dx * dx + dy * dy) - radius
    dist_norm = dist / (min(ww, hh) * 0.5)

    inside = dist <= 0

    # Bordo arrotondato: la normale si inclina verso l'esterno negli ultimi
    # pixel prima del bordo.
    bevel = np.clip((dist_norm + 0.16) / 0.16, 0, 1)
    gx = np.gradient(np.clip(-dist_norm, -1, 1), axis=1)
    gy = np.gradient(np.clip(-dist_norm, -1, 1), axis=0)
    nx, ny, nz = normalize_map(-gx * 26.0 * bevel, -gy * 26.0 * bevel, np.ones_like(gx))

    diffuse = np.clip(nx * LIGHT[0] + ny * LIGHT[1] + nz * LIGHT[2], 0, 1)
    fill = np.clip(nx * FILL[0] + ny * FILL[1] + nz * FILL[2], 0, 1)

    grain = value_noise((hh, ww), scale=max(4, ww // 30), octaves=3) - 0.5

    shade = (0.42 if not pressed else 0.30) + 0.62 * diffuse + 0.24 * fill

    view = np.array([0.0, 0.0, 1.0])
    hv = LIGHT + view
    hv /= np.linalg.norm(hv)
    spec = np.clip(nx * hv[0] + ny * hv[1] + nz * hv[2], 0, 1) ** 16

    out = np.zeros((hh, ww, 4))
    for c in range(3):
        v = tint[c] * shade + spec * (0.12 if pressed else 0.20) + grain * 0.028
        if pressed:
            v *= 0.78
        out[..., c] = np.clip(v, 0, 1)

    edge_px = 1.5 / (min(ww, hh) * 0.5)
    coverage = np.clip(-dist_norm / edge_px, 0, 1)

    out[..., :3] *= coverage[..., None]
    out[..., 3] = coverage

    return downsample(out)


def lcd_panel(w=260, h=64):
    """Display retroilluminato: vetro scuro, retroilluminazione ambrata non
    uniforme, riflesso della stanza in alto."""
    ww, hh = w * SS, h * SS

    yy, xx = np.mgrid[0:hh, 0:ww]

    # Retroilluminazione: due LED ai lati, quindi luminanza a sella.
    bx = (xx / ww - 0.5) * 2.0
    by = (yy / hh - 0.5) * 2.0
    backlight = 0.55 + 0.45 * np.exp(-(bx ** 2) * 1.1) * np.exp(-(by ** 2) * 0.6)
    backlight += 0.18 * np.exp(-((bx + 0.9) ** 2) * 8.0)
    backlight += 0.18 * np.exp(-((bx - 0.9) ** 2) * 8.0)

    base = np.array([0.055, 0.075, 0.062])
    glow = np.array([0.10, 0.34, 0.20])

    out = np.zeros((hh, ww, 4))
    for c in range(3):
        out[..., c] = base[c] + glow[c] * backlight * 0.55

    # Griglia di pixel: leggerissima, altrimenti sembra una zanzariera.
    grid = (np.sin(xx * math.pi / (SS * 2)) * 0.5 + 0.5) * (np.sin(yy * math.pi / (SS * 2)) * 0.5 + 0.5)
    out[..., :3] *= (0.955 + 0.045 * grid)[..., None]

    # Riflesso della stanza: banda diagonale nella metà superiore.
    refl = np.clip(1.0 - (yy / hh) * 2.4, 0, 1) ** 1.6
    diag = np.clip(1.0 - np.abs((xx / ww) * 1.4 - (yy / hh) * 0.5 - 0.25) * 3.0, 0, 1)
    out[..., :3] += (refl * diag * 0.10)[..., None]

    # Cornice incassata.
    frame = int(3 * SS)
    out[:frame, :, :3] *= 0.25
    out[-frame:, :, :3] *= 0.45
    out[:, :frame, :3] *= 0.30
    out[:, -frame:, :3] *= 0.35

    out[..., 3] = 1.0
    return downsample(out)


def orange_band(w=1400, h=46):
    """Banda arancione serigrafata su metallo, con leggera usura ai bordi."""
    ww, hh = w * SS, h * SS

    grain = value_noise((hh, ww), scale=max(4, ww // 200), octaves=3) - 0.5

    base = np.array([0.83, 0.32, 0.05])

    out = np.zeros((hh, ww, 4))
    for c in range(3):
        out[..., c] = np.clip(base[c] * (1.0 + grain * 0.10), 0, 1)

    # Gradiente: la serigrafia su una superficie leggermente curva.
    grad = np.linspace(1.12, 0.86, hh)[:, None]
    out[..., :3] *= grad[..., None]

    # Bordi: la vernice è più sottile e lascia intravedere il metallo.
    edge = int(1.6 * SS)
    out[:edge, :, :3] *= 1.25
    out[-edge:, :, :3] *= 0.72

    out[..., 3] = 1.0
    return downsample(out)


def slider_track(w=22, h=180):
    """Guida incassata per fader verticali."""
    ww, hh = w * SS, h * SS
    yy, xx = np.mgrid[0:hh, 0:ww]

    cx = (ww - 1) / 2.0
    dx = np.abs(xx - cx) / (ww * 0.5)

    # Sezione a U: pareti che salgono ai lati.
    depth = np.clip(1.0 - dx ** 2, 0, 1)

    nx = np.gradient(depth, axis=1) * 30.0
    ny = np.zeros_like(nx)
    nx, ny, nz = normalize_map(nx, ny, np.ones_like(nx))

    diffuse = np.clip(nx * LIGHT[0] + ny * LIGHT[1] + nz * LIGHT[2], 0, 1)
    er, eg, eb = environment(nx, ny, nz)

    ao = 0.25 + 0.55 * (1.0 - depth)

    out = np.zeros((hh, ww, 4))
    out[..., 0] = np.clip(0.10 * ao + diffuse * 0.20 + er * 0.16, 0, 1)
    out[..., 1] = np.clip(0.10 * ao + diffuse * 0.20 + eg * 0.16, 0, 1)
    out[..., 2] = np.clip(0.11 * ao + diffuse * 0.20 + eb * 0.17, 0, 1)

    coverage = np.clip((1.0 - dx) * (ww * 0.5) / 1.5, 0, 1)
    out[..., :3] *= coverage[..., None]
    out[..., 3] = coverage

    return downsample(out)


def slider_thumb(w=40, h=22):
    """Cursore del fader: corpo scuro con inserto metallico centrale."""
    ww, hh = w * SS, h * SS
    yy, xx = np.mgrid[0:hh, 0:ww]

    radius = min(ww, hh) * 0.22
    dx = np.maximum(np.abs(xx - (ww - 1) / 2.0) - (ww / 2.0 - radius), 0)
    dy = np.maximum(np.abs(yy - (hh - 1) / 2.0) - (hh / 2.0 - radius), 0)
    dist = np.sqrt(dx * dx + dy * dy) - radius
    dn = dist / (min(ww, hh) * 0.5)

    gx = np.gradient(np.clip(-dn, -1, 1), axis=1)
    gy = np.gradient(np.clip(-dn, -1, 1), axis=0)
    nx, ny, nz = normalize_map(-gx * 22.0, -gy * 22.0, np.ones_like(gx))

    diffuse = np.clip(nx * LIGHT[0] + ny * LIGHT[1] + nz * LIGHT[2], 0, 1)
    er, eg, eb = environment(nx, ny, nz)

    out = np.zeros((hh, ww, 4))
    shade = 0.30 + 0.60 * diffuse

    for c, e in enumerate((er, eg, eb)):
        out[..., c] = np.clip(0.13 * shade + e * 0.22, 0, 1)

    # Inserto metallico: banda centrale orizzontale, cromata.
    band = np.abs(yy - (hh - 1) / 2.0) < hh * 0.13
    for c, e in enumerate((er, eg, eb)):
        out[..., c] = np.where(band, np.clip(0.55 * shade + e * 0.70, 0, 1), out[..., c])

    coverage = np.clip(-dn * (min(ww, hh) * 0.5) / 1.5, 0, 1)
    out[..., :3] *= coverage[..., None]
    out[..., 3] = coverage

    return downsample(out)


# =============================================================================
# Main
# =============================================================================

def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else \
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Assets")
    out_dir = os.path.abspath(out_dir)
    os.makedirs(out_dir, exist_ok=True)

    print(f"Generazione texture in {out_dir}")

    print("\n[1/6] Pannelli")
    save(brushed_aluminium(700, 260), os.path.join(out_dir, "panel_metal.png"))
    save(abs_plastic(700, 340), os.path.join(out_dir, "panel_dark.png"))
    save(abs_plastic(320, 200, colour=(0.055, 0.058, 0.064), sheen=0.10),
         os.path.join(out_dir, "panel_module.png"))
    save(orange_band(700, 24), os.path.join(out_dir, "band_orange.png"))

    print("\n[2/6] Knob (sprite sheet)")
    save(knob_sheet(96, frames=65, style="chrome"), os.path.join(out_dir, "knob_chrome.png"))
    save(knob_sheet(64, frames=65, style="dark"), os.path.join(out_dir, "knob_dark.png"))
    save(knob_sheet(72, frames=65, style="orange"), os.path.join(out_dir, "knob_orange.png"))

    print("\n[3/6] Viti")
    save(screw(30), os.path.join(out_dir, "screw.png"))

    print("\n[4/6] LED")
    save(led(24, (1.0, 0.16, 0.08), on=True), os.path.join(out_dir, "led_red_on.png"))
    save(led(24, (1.0, 0.16, 0.08), on=False), os.path.join(out_dir, "led_red_off.png"))
    save(led(24, (1.0, 0.55, 0.06), on=True), os.path.join(out_dir, "led_amber_on.png"))
    save(led(24, (1.0, 0.55, 0.06), on=False), os.path.join(out_dir, "led_amber_off.png"))
    save(led(24, (0.25, 1.0, 0.35), on=True), os.path.join(out_dir, "led_green_on.png"))
    save(led(24, (0.25, 1.0, 0.35), on=False), os.path.join(out_dir, "led_green_off.png"))

    print("\n[5/6] Tasti e fader")
    save(button(72, 30, pressed=False), os.path.join(out_dir, "button_up.png"))
    save(button(72, 30, pressed=True), os.path.join(out_dir, "button_down.png"))
    save(slider_track(18, 160), os.path.join(out_dir, "slider_track.png"))
    save(slider_thumb(34, 18), os.path.join(out_dir, "slider_thumb.png"))

    print("\n[6/6] Display")
    save(lcd_panel(300, 70), os.path.join(out_dir, "lcd.png"))

    total = sum(os.path.getsize(os.path.join(out_dir, f))
                for f in os.listdir(out_dir) if f.endswith(".png"))
    print(f"\nTotale: {total / 1024 / 1024:.2f} MB")


if __name__ == "__main__":
    main()
