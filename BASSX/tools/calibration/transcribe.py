# 303 transcription on the 16th grid + per-note timbre features.
import sys, json, numpy as np
from scipy.io import wavfile
import warnings; warnings.filterwarnings('ignore')

def load(p):
    sr, x = wavfile.read(p); x = x.astype(np.float64)
    if x.ndim > 1: x = x.mean(1)
    return sr, x / (np.abs(x).max() + 1e-12)

def env_db(x, sr, hop):
    n = len(x) // hop
    e = np.sqrt(np.mean(x[: n * hop].reshape(n, hop) ** 2, axis=1))
    return 20 * np.log10(e + 1e-7)

def pitch_hps(seg, sr):
    w = np.hanning(len(seg)); S = np.abs(np.fft.rfft(seg * w, 1 << 16)); fr = np.fft.rfftfreq(1 << 16, 1 / sr)
    hps = np.log(S + 1e-9).copy(); lo = np.searchsorted(fr, 30); hi = np.searchsorted(fr, 400)
    acc = np.zeros(hi - lo)
    for k in range(1, 6):
        idx = (np.arange(lo, hi) * k)
        acc += np.log(S[idx] + 1e-9)
    return fr[lo + np.argmax(acc)]

def harm_profile(seg, sr, f0, nh=24):
    w = np.hanning(len(seg)); N = 1 << 15; S = np.abs(np.fft.rfft(seg * w, N)); fr = np.fft.rfftfreq(N, 1 / sr)
    out = []
    for k in range(1, nh + 1):
        f = k * f0
        if f > sr / 2 - 500: out.append(-120.0); continue
        i = int(round(f / (sr / N))); out.append(20 * np.log10(S[max(i - 4, 0): i + 5].max() + 1e-9))
    out = np.array(out); return out - out.max()

def transcribe(path, bpm):
    sr, x = load(path); hop = int(0.0025 * sr); e = env_db(x, sr, hop); top = np.percentile(e, 99)
    step = 60 / bpm / 4; nsteps = int(len(x) / sr / step)
    on = e > top - 18
    from track import yin_track
    yhop = int(0.005 * sr); f0 = yin_track(x, sr, yhop)
    gmed = np.median(f0[(f0 > 30) & (f0 < 400)])
    def at(t): return on[min(int(t * sr / hop), len(on) - 1)]
    notes = []
    s = 0
    while s < nsteps:
        t0 = s * step
        if not at(t0 + 0.2 * step): s += 1; continue
        # extend through ties: sound continues across the next step boundary
        s_end = s
        def no_dip(b):   # tie: level never drops in the 20 ms before the next step
            i0 = int((b - 0.02) * sr / hop); i1 = int(b * sr / hop)
            return i1 < len(e) and e[i0:i1].min() > top - 12
        while s_end + 1 < nsteps and no_dip((s_end + 1) * step):
            s_end += 1
        # gate end = last 'on' frame before the next gap
        k = int((s_end * step + 0.05 * step) * sr / hop)
        while k < len(on) and on[k]: k += 1
        t1 = k * hop / sr
        # pitch per step (slides change pitch inside a tie)
        seq = []
        for ss in range(s, s_end + 1):
            a = int((ss * step + 0.35 * step) * sr); b = int((ss * step + 0.85 * step) * sr) if ss < s_end else int(min(t1 - 0.005, (ss + 0.85) * step) * sr)
            if b - a < 1024: a = int(ss * step * sr); b = a + 2048
            tr = f0[int(a / yhop): int(b / yhop)]; tr = tr[(tr > 30) & (tr < 400)]
            if len(tr) >= 3: f = np.median(tr)
            else:   # HPS fallback: resonance can win, fold to the octave/fifth nearest the line's centre
                f = pitch_hps(x[a:b], sr)
                cands = [f / k for k in (1, 2, 3, 4)]
                f = min(cands, key=lambda c: abs(np.log2(c / gmed)))
            seq.append(int(round(69 + 12 * np.log2(f / 440.0))))
        a = int(t0 * sr); pk = e[int(t0 * sr / hop): int((t0 + 0.04) * sr / hop)].max()
        notes.append(dict(t=t0, steps=s_end - s + 1, gate=t1 - t0, midi=seq, peak=float(pk)))
        s = s_end + 1
    peaks = np.array([n['peak'] for n in notes])
    # accent: level jump AND brightness jump vs neighbours
    cents = []
    for n in notes:
        a = int(n['t'] * sr); seg = x[a: a + int(0.04 * sr)]
        S = np.abs(np.fft.rfft(seg * np.hanning(len(seg)))); fr = np.fft.rfftfreq(len(seg), 1 / sr)
        cents.append((S * fr).sum() / (S.sum() + 1e-12))
    cents = np.array(cents); medp = np.median(peaks); medc = np.median(cents)
    for n, p, c in zip(notes, peaks, cents):
        n['accent'] = bool(p > medp + 1.5 or c > 1.35 * medc)
    return sr, x, notes

def to_pattern(notes, length):
    ev = []
    for n in notes:
        st = len(n['midi']); dt = n['gate'] / st if st > 1 else n['gate']
        stepdur = n['gate'] / max(st, 1)
        for i, m in enumerate(n['midi']):
            # tied steps overlap the next note-on by 5 ms -> legato slide in BASSX
            t = n['t'] + i * (n['gate'] if st == 1 else 0) if False else None
        # steps of a tie: note-on at each step start, each lasting until the next starts (+overlap)
        step = (n['gate'] if st == 1 else None)
    return ev

if __name__ == '__main__':
    path, bpm, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
    sr, x, notes = transcribe(path, bpm)
    step = 60 / bpm / 4
    ev = []
    for n in notes:
        st = len(n['midi'])
        for i, m in enumerate(n['midi']):
            t = n['t'] + i * step
            last = i == st - 1
            dur = (n['t'] + n['gate'] - t) if last else step + 0.005
            ev.append([round(t, 5), round(max(dur, 0.01), 5), m, 127 if (n['accent'] and i == 0) else 90])
    json.dump(dict(notes=ev, length=len(x) / sr, bpm=bpm), open(out, 'w'))
    acc = sum(n['accent'] for n in notes); ties = sum(len(n['midi']) > 1 for n in notes)
    print(f"{len(notes)} notes, {acc} accents, {ties} ties/slides, gate/step median {np.median([n['gate']/len(n['midi'])/step for n in notes]):.2f}")
    print([ (round(n['t'],3), n['midi'], n['accent']) for n in notes[:16]])
