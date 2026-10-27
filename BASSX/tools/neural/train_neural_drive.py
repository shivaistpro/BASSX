"""
BASSX — addestramento del modello "Neural Drive".

Cosa modella: un preamp a diodi con memoria (accoppiamento capacitivo in
ingresso + clipper a diodi asimmetrico con condensatore di carico), cioè lo
stadio di saturazione che si mette davanti al filtro di una bass line per
l'overdrive "alla Devil Fish". Il circuito è descritto dall'ODE

    C dy/dt = (u - y)/R - Is_p (exp(y/(n Vt)) - 1) + Is_n (exp(-y/(n Vt)) - 1)

con u = ingresso dopo il passa-alto di accoppiamento e guadagno di drive.
Un'ODE rigida come questa richiede Newton-Raphson a ogni campione; una GRU
piccola (8 stati) la approssima a costo fisso e senza iterazioni. È la stessa
idea dei modelli neurali di amplificatori (Wright et al., 2019): qui il
dataset è generato dal modello numerico del circuito, non da registrazioni di
un esemplare hardware — per quello servirebbero catture input/output reali.

Ingressi della rete: [campione, drive 0..1]. Uscita: campione.
Esporta i pesi in Source/DSP/NeuralDriveWeights.h.
"""
import math, sys, pathlib
import numpy as np
import torch, torch.nn as nn

SR = 48000
torch.set_num_threads(2)
rng = np.random.default_rng(1234)

# ---------------------------------------------------------------- circuito
R, C = 2200.0, 22e-9          # RC del carico: polo ~3.3 kHz
ISP, ISN, NVT = 2.5e-9, 6e-9, 1.8 * 0.02585   # diodi asimmetrici
HP_HZ = 30.0

def circuit(x, drive):
    """Simula il circuito (Eulero implicito + Newton) con oversampling 4x."""
    os_ = 4
    dt = 1.0 / (SR * os_)
    a_hp = math.exp(-2 * math.pi * HP_HZ / (SR * os_))
    gain = 1.0 + 39.0 * drive ** 2
    y = 0.0; hp_x = 0.0; hp_y = 0.0
    out = np.zeros_like(x)
    k = dt / C
    for n, xn in enumerate(x):
        for _ in range(os_):
            hp_y = a_hp * (hp_y + xn - hp_x); hp_x = xn   # passa-alto 1 polo
            u = hp_y * gain
            yn = y
            for _ in range(6):   # Newton su y_{n+1}
                ep = math.exp(min(yn / NVT, 60)); en = math.exp(min(-yn / NVT, 60))
                f = yn - y - k * ((u - yn) / R - ISP * (ep - 1) + ISN * (en - 1))
                df = 1 + k * (1 / R + ISP * ep / NVT + ISN * en / NVT)
                yn -= f / df
            y = yn
        out[n] = y
    # normalizzazione: i diodi saturano intorno a ~0.6 V
    return out / 0.6

# ---------------------------------------------------------------- segnali
def bass_signal(n):
    """Linee di basso sintetiche: dente di sega/quadra con sweep di filtro,
    più seni e rumore — la varietà che il modello vedrà nel plugin."""
    out = np.zeros(n); t = 0
    while t < n:
        L = int(rng.integers(1500, 9000))
        f = 30 * 2 ** rng.uniform(0, 5)
        ph = np.cumsum(np.full(L, f / SR)) % 1.0
        kind = rng.integers(0, 4)
        if kind == 0: s = 2 * ph - 1
        elif kind == 1: s = np.where(ph < 0.48, 1.0, -1.0)
        elif kind == 2: s = np.sin(2 * np.pi * ph)
        else: s = rng.normal(0, 0.5, L)
        # inviluppo + lowpass variabile (one pole) per varietà spettrale
        env = np.exp(-np.arange(L) / (SR * rng.uniform(0.05, 0.6)))
        fc = 200 * 2 ** rng.uniform(0, 6)
        a = math.exp(-2 * math.pi * fc / SR); z = 0.0; lp = np.zeros(L)
        for i in range(L):
            z = (1 - a) * s[i] + a * z; lp[i] = z
        amp = rng.uniform(0.05, 1.0)
        seg = lp * env * amp
        out[t:t + L] = seg[: n - t]
        t += L
    return out

print("genero il dataset...", flush=True)
SEQ = 2048; NSEQ = int(sys.argv[1]) if len(sys.argv) > 1 else 160
X = np.zeros((NSEQ, SEQ, 2), np.float32); Y = np.zeros((NSEQ, SEQ, 1), np.float32)
for i in range(NSEQ):
    d = rng.uniform(0, 1)
    x = bass_signal(SEQ + 512)
    y = circuit(x, d)
    X[i, :, 0] = x[512:]; X[i, :, 1] = d; Y[i, :, 0] = y[512:]   # 512 di warm-up scartati
    if i % 20 == 0: print(f"  {i}/{NSEQ}", flush=True)

Xt = torch.tensor(X); Yt = torch.tensor(Y)
ntr = int(NSEQ * 0.85)

class Model(nn.Module):
    def __init__(s, H=8):
        super().__init__()
        s.gru = nn.GRU(2, H, batch_first=True)
        s.out = nn.Linear(H, 1)
    def forward(s, x):
        h, _ = s.gru(x)
        return s.out(h) + x[..., :1] * 0.0   # nessun salto residuo: il drive cambia il guadagno

torch.manual_seed(0)
HID = int(sys.argv[3]) if len(sys.argv) > 3 else 8
m = Model(HID)
opt = torch.optim.Adam(m.parameters(), lr=4e-3)
EPOCHS = int(sys.argv[2]) if len(sys.argv) > 2 else 400
sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, T_max=EPOCHS)

def esr(pred, tgt):   # error-to-signal ratio, la metrica dei modelli di amp
    return ((pred - tgt) ** 2).sum() / ((tgt ** 2).sum() + 1e-9)

print("addestro...", flush=True)
WARM = 256
for ep in range(EPOCHS):
    m.train()
    idx = torch.randperm(ntr)[:32]
    pred = m(Xt[idx])
    loss = esr(pred[:, WARM:], Yt[idx][:, WARM:])
    opt.zero_grad(); loss.backward()
    nn.utils.clip_grad_norm_(m.parameters(), 1.0)
    opt.step(); sched.step()
    if ep % 25 == 0 or ep == EPOCHS - 1:
        m.eval()
        with torch.no_grad():
            v = esr(m(Xt[ntr:])[:, WARM:], Yt[ntr:][:, WARM:]).item()
        print(f"  epoca {ep:4d}  train ESR {loss.item():.4f}  valid ESR {v:.4f}", flush=True)

m.eval()
with torch.no_grad():
    v = esr(m(Xt[ntr:])[:, WARM:], Yt[ntr:][:, WARM:]).item()

# ---------------------------------------------------------------- export
g = m.gru
def arr(t): return ", ".join(f"{float(v):.8e}f" for v in t.detach().numpy().ravel())
H = g.hidden_size
hdr = f"""/*
  ==============================================================================

    BASSX — NeuralDriveWeights.h

    GENERATO da tools/neural/train_neural_drive.py — non modificare a mano.

    GRU {H} stati, ingressi [campione, drive], uscita lineare.
    Error-to-signal ratio sul set di validazione: {v:.4f}

    Ordine dei gate (convenzione PyTorch): reset, update, new.

  ==============================================================================
*/

#pragma once

namespace bassx::neural
{{
inline constexpr int kHidden = {H};
inline constexpr float kValidationESR = {v:.6f}f;

// weight_ih: [3H x 2]
inline constexpr float Wih[{3*H*2}] = {{ {arr(g.weight_ih_l0)} }};
// weight_hh: [3H x H]
inline constexpr float Whh[{3*H*H}] = {{ {arr(g.weight_hh_l0)} }};
inline constexpr float Bih[{3*H}] = {{ {arr(g.bias_ih_l0)} }};
inline constexpr float Bhh[{3*H}] = {{ {arr(g.bias_hh_l0)} }};
// uscita: [1 x H] + bias
inline constexpr float Wout[{H}] = {{ {arr(m.out.weight)} }};
inline constexpr float Bout = {float(m.out.bias.item()):.8e}f;

}} // namespace bassx::neural
"""
dst = pathlib.Path(__file__).resolve().parents[2] / "Source" / "DSP" / "NeuralDriveWeights.h"
dst.write_text(hdr)

# vettore di test per la verifica dell'inferenza C++
x = bass_signal(4096).astype(np.float32); d = 0.7
with torch.no_grad():
    yp = m(torch.tensor(np.stack([x, np.full_like(x, d)], -1))[None]).numpy().ravel()
np.savetxt(pathlib.Path(__file__).parent / "reference_io.txt", np.stack([x, yp], 1), fmt="%.8e")
print(f"FINE  valid ESR {v:.4f}  -> {dst}")
