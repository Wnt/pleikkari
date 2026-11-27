import io, sys
import numpy as np

def filled(mo, axis):
    """The picture's motion per pair, with pairs that could not be measured given the speed of
    the measured ones around them (a fast turn blurs, and is then not measured: counted as
    standing still it made the truth too small)."""
    import numpy as _np
    dt = (mo[:, 1] - mo[:, 0]) / 1e6
    ok = mo[:, 4] > 0
    v = mo[:, axis] / _np.maximum(dt, 1e-6)
    if ok.sum() >= 2:
        v = _np.interp(mo[:, 1], mo[ok, 1], v[ok])
    return v * dt

D = sys.argv[1]; lag = float(sys.argv[2]); count = int(sys.argv[3]) if len(sys.argv) > 3 else 1
def load(p, n, skip=1):
    L = open(p).read().split('\n')[skip:-1]
    return np.loadtxt(io.StringIO('\n'.join(l for l in L if l.count(',') == n)), delimiter=',')
mo, pd, st = load(D + '/off-motion.csv', 10), load(D + '/off-pred.csv', 6), load(D + '/off-stick.csv', 3)
n = min(len(mo), len(pd)); mo, pd = mo[:n], pd[:n]
pic = mo[:, 1]; cum = np.cumsum(filled(mo, 2))
truth = np.interp(pic + (lag + 8) * 1000, pic, cum) - cum; err = pd[:, 1] - truth
e = abs(err).copy(); e[:int(n * 0.3)] = 0
for c in range(count):
    k = int(np.argmax(e)); e[max(0, k - 60):k + 60] = 0
    print('-- error %+.3f at picture %d' % (err[k], k))
    for j in range(max(0, k - 12), min(n, k + 8)):
        i = max(0, np.searchsorted(st[:, 0], pic[j] - lag * 1000) - 1); i2 = max(0, np.searchsorted(st[:, 0], pic[j]) - 1)
        print('%+5.0f ms: stick then %+.2f now %+.2f (mode %d)  speed %+.2f%s  truth %+.4f shown %+.4f err %+.4f expl %.2f' % ((pic[j] - pic[k]) / 1000, st[i, 1], st[i2, 1], st[i2, 3], mo[j, 2] / ((mo[j, 1] - mo[j, 0]) / 1e6), '' if mo[j, 4] > 0 else 'x', truth[j], pd[j, 1], err[j], pd[j, 6]))
