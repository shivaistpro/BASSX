# Continuous tracking: f0 (YIN, 5 ms hop), RMS env, spectral centroid; plot for one file.
import sys, numpy as np, matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
from scipy.io import wavfile
from scipy.signal import spectrogram
import warnings; warnings.filterwarnings('ignore')

def yin_track(x, sr, hop, fmin=35, fmax=500, win=2048):
    tmax = int(sr / fmin); tmin = int(sr / fmax); out = []
    for c in range(0, len(x) - win - tmax, hop):
        w = x[c:c + win + tmax]
        # difference function via FFT
        d = np.zeros(tmax)
        xx = w[:win]
        for t in range(tmin - 1, tmax):
            pass
        ac = np.correlate(w, xx, 'valid')[:tmax]
        e0 = np.sum(xx ** 2); csum = np.cumsum(np.concatenate([[0], w ** 2]))
        et = csum[np.arange(tmax) + win] - csum[np.arange(tmax)]
        d = e0 + et - 2 * ac
        cm = np.ones(tmax); cm[1:] = d[1:] * np.arange(1, tmax) / (np.cumsum(d[1:]) + 1e-12)
        f = 0.0
        idx = np.where(cm[tmin:] < 0.2)[0]
        if len(idx):
            t = idx[0] + tmin
            while t + 1 < tmax and cm[t + 1] < cm[t]: t += 1
            a, b, cc = cm[t - 1], cm[t], cm[t + 1] if t + 1 < tmax else cm[t]
            off = 0.5 * (a - cc) / (a - 2 * b + cc + 1e-12)
            f = sr / (t + off)
        out.append(f)
    return np.array(out)

def load(p):
    sr, x = wavfile.read(p); x = x.astype(np.float64)
    if x.ndim > 1: x = x.mean(1)
    return sr, x / (np.abs(x).max() + 1e-12)

if __name__ == '__main__':
    p = sys.argv[1]; sr, x = load(p); hop = int(0.005 * sr)
    x = x[: int(sr * float(sys.argv[3]) if len(sys.argv) > 3 else sr * 3.5)]
    f0 = yin_track(x, sr, hop)
    env = np.array([np.sqrt(np.mean(x[i:i + hop] ** 2)) for i in range(0, len(x) - hop, hop)])
    fig, ax = plt.subplots(3, 1, figsize=(16, 9), sharex=True)
    f, t, S = spectrogram(x, sr, nperseg=1024, noverlap=1024 - 128)
    ax[0].pcolormesh(t, f, 10 * np.log10(S + 1e-12), vmin=-90, shading='auto'); ax[0].set_ylim(0, 5000)
    tt = np.arange(len(f0)) * hop / sr; ax[1].plot(tt, np.where(f0 > 0, f0, np.nan)); ax[1].set_ylabel('f0')
    te = np.arange(len(env)) * hop / sr; ax[2].plot(te, 20 * np.log10(env + 1e-6)); ax[2].set_ylabel('dB')
    bpm = float(p.split('_')[-1][:-4]); step = 60 / bpm / 4
    for a in ax:
        for k in range(int(len(x) / sr / step)): a.axvline(k * step, color='w' if a is ax[0] else 'gray', lw=0.3)
    plt.tight_layout(); plt.savefig(sys.argv[2], dpi=70)
