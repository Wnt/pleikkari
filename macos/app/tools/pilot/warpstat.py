"""warpstat.py RUNDIR [SECONDS]: what the live warp did in the last SECONDS of a recording session."""
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

D = sys.argv[1]
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 60
L = open(D + '/rec-motion.csv').read().split('\n')[1:-1]
mo = np.loadtxt(io.StringIO('\n'.join(l for l in L if l.count(',') == 10)), delimiter=',')
mo = mo[mo[:, 1] > mo[-1, 1] - secs * 1e6]
pic, dx, valid, wx = mo[:, 1], mo[:, 2], mo[:, 4], mo[:, 5]
dt = (mo[:, 1] - mo[:, 0]) / 1e6
L = float(np.median(mo[:, 7]))
cum = np.cumsum(filled(mo, 2))
truth = np.interp(pic + (L + 8) * 1000, pic, cum) - cum
rms = lambda a: float(np.sqrt((a ** 2).mean()) * 100)
print('%d pictures in %.0f s (interval p50 %.0f ms, max %.0f ms), valid %.2f' % (len(mo), (pic[-1] - pic[0]) / 1e6, np.median(dt) * 1000, dt.max() * 1000, valid.mean()))
print('delay %.0f ms; explained p50 %.2f; lag to hide %.3f %% of the width (rms), left with the warp %.3f %% -> %.0f %% removed' % (
    L, np.median(mo[:, 10]), rms(truth), rms(wx - truth), 100 * (1 - rms(wx - truth) / max(1e-9, rms(truth)))))
