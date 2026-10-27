# Fit BASSX filter/env knobs so its average 16th-note spectrum matches the 303 recordings.
import sys, os, json, subprocess, tempfile, numpy as np
from scipy.io import wavfile
from scipy.optimize import minimize
from transcribe import harm_profile
import warnings; warnings.filterwarnings('ignore')
# Percorsi dal proprio PC (vedi README): BASSX_MATCH = eseguibile BassXMatch, BASSX_REFS = cartella dei WAV del 303
BX = os.environ.get('BASSX_MATCH', 'BassXMatch')
REFS = os.environ.get('BASSX_REFS', 'refs')
TMP = tempfile.gettempdir()
FILES = ['Tb 303 Tekno Club P1_145.wav', 'Tb 303 Uber P1_150.wav', 'Tb 303 Sweet P1_145.wav', 'Tb 303 Off Saw P1_145.wav',
         'Tb 303 Ohm P1_145.wav', 'Tb 303 Paralyze P1_145.wav', 'Tb 303 Techno Lullaby P1_145.wav', 'Tb 303 Move 2 P1_145.wav']
NH = 20
def profiles(wav, pat):
    sr, x = wavfile.read(wav); x = x.astype(np.float64); x = x.mean(1) if x.ndim > 1 else x; x /= np.abs(x).max() + 1e-12
    P = json.load(open(pat)); notes = P['notes']; E, L = [], []
    for i, (t, d, m, v) in enumerate(notes):
        if v >= 100 or d > 0.1: continue                     # plain, non-accented, non-tied
        f0 = 440 * 2 ** ((m - 69) / 12); a = int(t * sr)
        he = harm_profile(x[a + int(.005 * sr): a + int(.025 * sr)], sr, f0, NH); hl = harm_profile(x[a + int(.04 * sr): a + int(.06 * sr)], sr, f0, NH)
        E.append(he - he[0]); L.append(hl - hl[0])
    return np.median(E, 0), np.median(L, 0)
REAL = {f: profiles(os.path.join(REFS, f), 'bx/' + f.replace('.wav', '.json')) for f in FILES}
def cost(params, preset='TB Original Saw', verbose=False):
    cut, reso, env, dec, drive = params
    args = [f'f1_cutoff={cut}', f'f1_reso={reso}', f'f1_env_amount={env}', f'env2_decay={dec}', f'f1_drive={drive}']
    tot = 0; rows = []
    for f in FILES:
        out = os.path.join(TMP, 'bassx_fit.wav')
        subprocess.run([BX, preset, 'bx/' + f.replace('.wav', '.json'), out] + args, capture_output=True, check=True)
        be, bl = profiles(out, 'bx/' + f.replace('.wav', '.json')); re_, rl = REAL[f]
        tot += np.abs(be - re_)[1:].mean() + np.abs(bl - rl)[1:].mean(); rows.append((be, bl))
    c = tot / len(FILES)
    if verbose: print('cost %.2f' % c, args)
    return c, rows
if __name__ == '__main__':
    x0 = [480.0, 0.62, 0.6, 0.2, 1.4]
    print('start', cost(x0, verbose=True)[0])
    # coarse grid on cutoff/reso first (the dominant knobs), then Nelder-Mead on all five
    best = (1e9, None)
    for cut in (300, 450, 650, 900, 1300):
        for reso in (0.5, 0.7, 0.85, 0.95):
            c = cost([cut, reso, 0.5, 0.25, 2.0])[0]
            if c < best[0]: best = (c, [cut, reso, 0.5, 0.25, 2.0])
    print('grid best', best)
    lo = np.array([60, 0.0, 0.0, 0.02, 1.0]); hi = np.array([5000, 1.0, 1.0, 2.0, 10.0])
    f = lambda z: cost(np.clip(z, lo, hi))[0]
    r = minimize(f, best[1], method='Nelder-Mead', options=dict(maxfev=70, xatol=1e-3, fatol=0.05, initial_simplex=None))
    xb = np.clip(r.x, lo, hi); c, rows = cost(xb, verbose=True)
    json.dump(dict(x=list(map(float, xb)), cost=float(c)), open('fit_result.json', 'w'))
    np.set_printoptions(precision=1, suppress=True, linewidth=200)
    re_ = np.median([REAL[f][0] for f in FILES], 0); be = np.median([r[0] for r in rows], 0)
    rl = np.median([REAL[f][1] for f in FILES], 0); bl = np.median([r[1] for r in rows], 0)
    print('303 early', re_); print('BX  early', be); print('303 late ', rl); print('BX  late ', bl)
