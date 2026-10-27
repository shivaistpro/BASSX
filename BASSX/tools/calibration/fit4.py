import json, itertools, subprocess, numpy as np
from scipy.optimize import minimize
import os
from fit import BX, TMP, profiles, REAL
FILES4 = ['Tb 303 Tekno Club P1_145.wav', 'Tb 303 Uber P1_150.wav', 'Tb 303 Sweet P1_145.wav', 'Tb 303 Off Saw P1_145.wav']
FILES8 = FILES4 + ['Tb 303 Ohm P1_145.wav', 'Tb 303 Paralyze P1_145.wav', 'Tb 303 Techno Lullaby P1_145.wav', 'Tb 303 Move 2 P1_145.wav']
NAMES = ['f1_cutoff', 'f1_reso', 'f1_env_amount', 'env2_decay', 'eq_low_gain', 'eq_low_freq', 'eq_high_gain', 'dist_drive']
LO = np.array([150, 0.3, 0.0, 0.05, -24, 40, -6, 0.0]); HI = np.array([4000, 0.98, 0.6, 1.5, 6, 400, 12, 1.0])
def cost(z, files=FILES4, verbose=False):
    z = np.clip(z, LO, HI); args = [f'{n}={v}' for n, v in zip(NAMES, z)] + ['dist_on=1', 'dist_algo=1', 'dist_mix=0.6', 'f1_bass_comp=0']
    tot = 0; rows = []
    for f in files:
        out = os.path.join(TMP, 'bassx_fit4.wav'); pat = 'bx/' + f.replace('.wav', '.json')
        subprocess.run([BX, 'Acid Classic', pat, out] + args, capture_output=True, check=True)
        be, bl = profiles(out, pat); re_, rl = REAL[f]
        w = np.ones(len(be)); w[1:8] = 2.0
        tot += (w * np.abs(be - re_))[1:].mean() + (w * np.abs(bl - rl))[1:].mean(); rows.append((be, bl))
    if verbose: print('cost %.2f' % (tot / len(files)), ' '.join(f'{n}={v:.3g}' for n, v in zip(NAMES, z)), flush=True)
    return tot / len(files), rows
if __name__ == '__main__':
    best = None
    for cut, reso, env, elg in itertools.product((450, 900, 1500), (0.85, 0.97), (0.0, 0.08, 0.16), (-6, -12, -18)):
        z = np.array([cut, reso, env, 0.3, elg, 140, 3, 0.2]); c = cost(z)[0]
        if best is None or c < best[0]: best = (c, z); print('grid', round(c, 2), z, flush=True)
    r = minimize(lambda z: cost(z)[0], best[1], method='Nelder-Mead', options=dict(maxfev=90, fatol=0.05,
                 initial_simplex=[best[1] + (np.eye(8)[i - 1] * 0.15 * np.maximum(np.abs(best[1]), 0.1) if i else 0) for i in range(9)]))
    c, rows = cost(r.x, FILES8, verbose=True)
    json.dump(dict(x=dict(zip(NAMES, map(float, np.clip(r.x, LO, HI)))), cost=c), open('fit4_result.json', 'w'), indent=1)
    np.set_printoptions(precision=1, suppress=True, linewidth=200)
    print('303 early', np.median([REAL[f][0] for f in FILES8], 0)); print('BX  early', np.median([q[0] for q in rows], 0))
    print('303 late ', np.median([REAL[f][1] for f in FILES8], 0)); print('BX  late ', np.median([q[1] for q in rows], 0))
