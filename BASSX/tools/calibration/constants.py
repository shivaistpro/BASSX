# Hardware constants of a 303 line: attack, gate-off fall, sustain droop, slide time/shape,
# accent level/brightness. Works on any render with the same 16th grid.
import sys, os, json, numpy as np
from transcribe import transcribe, load, env_db
from track import yin_track
import warnings; warnings.filterwarnings('ignore')

def cross_time(e, i0, lvl, direction, hop_ms, maxn):
    for k in range(maxn):
        i = i0 + direction * k
        if i < 0 or i >= len(e): return None
        if (direction > 0 and e[i] >= lvl) or (direction < 0 and e[i] >= lvl): return k * hop_ms
    return None

def measure(path, bpm):
    sr, x, notes = transcribe(path, bpm)
    hop = int(0.0005 * sr); e = env_db(x, sr, hop); hop_ms = 0.5
    step = 60 / bpm / 4
    out = dict(attack=[], fall=[], droop=[], glide=[], glide_shape=[], acc_db=[], acc_bright=[], norm_bright=[], gate=[])
    def cent(a, b):
        seg = x[a:b]; S = np.abs(np.fft.rfft(seg * np.hanning(len(seg)))); fr = np.fft.rfftfreq(len(seg), 1 / sr)
        return (S * fr).sum() / (S.sum() + 1e-12)
    levels = []
    for n in notes:
        i0 = int(n['t'] * sr / hop); i1 = int((n['t'] + n['gate']) * sr / hop)
        if i1 - i0 < 40: continue
        pk = np.max(e[i0: i0 + 60])  # first 30 ms
        # attack: frames from -20 to -3 dB below the early peak
        seg = e[i0: i0 + 60]
        a20 = np.argmax(seg > pk - 20); a3 = np.argmax(seg > pk - 3); out['attack'].append((a3 - a20) * hop_ms)
        # fall at gate off: last frame above pk-3 before i1+20, then first below pk-20
        tail = e[i1 - 80: i1 + 60]
        if len(tail) == 140:
            j3 = 80 - np.argmax(tail[:80][::-1] > pk - 3) if (tail[:80] > pk - 3).any() else None
            if j3 is not None:
                k20 = np.argmax(tail[j3:] < pk - 20)
                if k20 > 0: out['fall'].append(k20 * hop_ms)
        if len(n['midi']) == 1:
            a = e[i0 + 20: i0 + 30].mean(); b = e[i0 + 110: i0 + 120].mean() if i1 - i0 > 125 else np.nan
            out['droop'].append(b - a)
            out['gate'].append(n['gate'] / step)
        c = cent(int(n['t'] * sr), int((n['t'] + 0.03) * sr))
        levels.append((n['accent'], pk, c))
    acc = [l for l in levels if l[0]]; nor = [l for l in levels if not l[0]]
    if acc and nor:
        out['acc_db'] = [np.median([l[1] for l in acc]) - np.median([l[1] for l in nor])]
        out['acc_bright'] = [np.median([l[2] for l in acc]) / np.median([l[2] for l in nor])]
    # slides: ties with a pitch change, 1 ms f0 track around the step boundary
    yh = int(0.001 * sr)
    for n in notes:
        for i in range(1, len(n['midi'])):
            d = n['midi'][i] - n['midi'][i - 1]
            if abs(d) < 2: continue
            b = n['t'] + i * step
            a0 = int((b - 0.03) * sr); a1 = int((b + 0.12) * sr)
            tr = yin_track(x[a0: a1 + 4096], sr, yh, win=1024)
            ok = (tr > 25) & (tr < 600)
            if ok.sum() < 40: continue
            f_start = 440 * 2 ** ((n['midi'][i - 1] - 69) / 12); st = 12 * np.log2(np.where(ok, tr, f_start) / f_start)
            prog = st / d
            idx = np.arange(len(tr))
            try:
                t10 = idx[(prog > 0.1) & ok][0]; t50 = idx[(prog > 0.5) & ok][0]; t90 = idx[(prog > 0.9) & ok][0]
            except IndexError: continue
            if t90 <= t10: continue
            out['glide'].append(float(t90 - t10))                        # ms (1 ms hop)
            out['glide_shape'].append(float((t50 - t10) / (t90 - t10)))  # exp (RC) ~0.32, linear 0.5
    return {k: (float(np.nanmedian(v)) if len(v) else None) for k, v in out.items()} | {'n_glides': len(out['glide'])}

if __name__ == '__main__':
    res = {}
    for p in sys.argv[1:]:
        bpm = float(p.split('_')[-1][:-4]); r = measure(p, bpm); res[os.path.basename(p)] = r
        print(os.path.basename(p)[:34].ljust(34), ' '.join(f"{k}={v:.2f}" if isinstance(v, float) else f"{k}={v}" for k, v in r.items()))
    keys = ['attack', 'fall', 'droop', 'gate', 'glide', 'glide_shape', 'acc_db', 'acc_bright']
    print('MEDIAN', {k: round(float(np.nanmedian([r[k] for r in res.values() if r[k] is not None])), 3) for k in keys})
    json.dump(res, open('constants.json', 'w'), indent=1)
