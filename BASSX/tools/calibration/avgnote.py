# Average note envelope + rest-tail templates (aligned on the 16th grid), real vs BASSX.
import sys, os, json, numpy as np, matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
from transcribe import transcribe
import warnings; warnings.filterwarnings('ignore')
def templates(files):
    T16, TR = [], []
    for p in files:
        bpm = float(p.split('_')[-1][:-4]); step = 60 / bpm / 4
        sr, x, notes = transcribe(p, bpm)
        w = int(0.003 * sr); pw = np.convolve(x ** 2, np.ones(w) / w, 'same')
        hop = int(0.001 * sr); n_step = 99
        for i, n in enumerate(notes):
            if len(n['midi']) != 1: continue
            a = int(n['t'] * sr); seg = pw[a: a + int(2 * step * sr): hop]
            if len(seg) < 2 * n_step: continue
            db = 10 * np.log10(seg + 1e-12); db -= np.median(db[5:40])
            nxt = notes[i + 1]['t'] if i + 1 < len(notes) else 1e9
            if abs(nxt - n['t'] - step) < 0.002: T16.append(db[:n_step])
            elif nxt - n['t'] > 1.9 * step: TR.append(db[: 2 * n_step])
    return np.median(T16, 0) if T16 else None, np.median(TR, 0) if TR else None, len(T16), len(TR)
REFS = os.environ.get('BASSX_REFS', 'refs')   # cartella dei WAV del 303
real = sorted(os.path.join(REFS, f) for f in os.listdir(REFS) if f.endswith('.wav'))
bx = ['bx/' + os.path.basename(f) for f in real]
r16, rR, n1, n2 = templates(real); b16, bR, m1, m2 = templates(bx)
print('notes real', n1, n2, 'bx', m1, m2)
def cross(c, lvl):
    k = 5
    while k < len(c) and c[k] > lvl: k += 1
    return k
for name, c in (('real 16th', r16), ('bx 16th', b16), ('real rest', rR), ('bx rest', bR)):
    print(name.ljust(10), 'ms to -3/-10/-20/-30 dB:', [cross(c, l) for l in (-3, -10, -20, -30)], ' attack ms to -3 from start:', int(np.argmax(c > -3)))
fig, ax = plt.subplots(1, 2, figsize=(14, 4))
ax[0].plot(r16, label='303'); ax[0].plot(b16, label='BASSX'); ax[0].set_title('16th note (next note on next step)'); ax[0].legend(); ax[0].set_ylim(-50, 6)
ax[1].plot(rR, label='303'); ax[1].plot(bR, label='BASSX'); ax[1].set_title('note followed by rest'); ax[1].legend(); ax[1].set_ylim(-70, 6)
for a in ax: a.set_xlabel('ms'); a.grid(alpha=.3)
plt.tight_layout(); plt.savefig('avgnote.png', dpi=70)
np.save('templates.npy', dict(r16=r16, rR=rR, b16=b16, bR=bR), allow_pickle=True)
