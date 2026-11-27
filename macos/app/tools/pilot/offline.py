"""offline.py RUNDIR [max_shift]: replay a recording session's stick and measured motion through the
warp model as built in dev.noindex, and say how well it would have done, by the kind of moment."""
import io, os, subprocess, sys
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

W = os.environ.get('PLK_WORKSPACE', os.path.expanduser('~/Documents/pleikkari-android'))
D = sys.argv[1]
def load(p, n):
    L = open(p).read().split('\n')[1:-1]
    return np.loadtxt(io.StringIO('\n'.join(l for l in L if l.count(',') == n)), delimiter=',')
mo, inp = load(D + '/rec-motion.csv', 10), load(D + '/rec-input.csv', 7)
with open(D + '/off-stick.csv', 'w') as f:
    f.write('t_us,x,y,aiming\n')
    for r in inp:
        f.write('%.0f,%.4f,%.4f,%d\n' % (r[0], r[6] / 32767, r[7] / 32767, (1 if r[2] > 100 else 0) + (2 if int(r[1]) & 256 else 0)))
with open(D + '/off-motion.csv', 'w') as f:
    f.write('h\n')
    np.savetxt(f, mo, delimiter=',', fmt='%.6f')
env = dict(os.environ)
if len(sys.argv) > 2:
    env['PLK_WARP_MAX_SHIFT'] = sys.argv[2]
out = subprocess.run([W + '/build/native-app/dev.noindex/macos/app/plk-warp-eval', D + '/off'], capture_output=True, text=True, env=env)
pd = np.loadtxt(io.StringIO(out.stdout), delimiter=',', skiprows=1)
n = min(len(pd), len(mo)); pd, mo = pd[:n], mo[:n]
pic, valid = mo[:, 1], mo[:, 4]
lag = float(np.median(pd[len(pd) // 2:, 3]))
st_t, st_x = inp[:, 0], inp[:, 6] / 32767
# seconds since the right stick last changed by more than 0.3, as seen at each picture
changes = [st_t[i] for i in range(1, len(inp)) if abs(st_x[i] - st_x[i - 1]) > 0.3 or abs(inp[i, 7] - inp[i - 1, 7]) > 9800]
changes = np.array(changes) if changes else np.array([0.0])
since = np.array([(t - changes[np.searchsorted(changes, t) - 1]) / 1e6 if np.searchsorted(changes, t) > 0 else 9 for t in pic])
half = pic > pic[0] + (pic[-1] - pic[0]) * 0.3
rms = lambda v: float(np.sqrt((v ** 2).mean()) * 100) if len(v) else 0.0
print('delay %.0f ms; %d pictures; max shift %s' % (lag, n, sys.argv[2] if len(sys.argv) > 2 else 'default'))
for axis, name in ((2, 'sideways'), (3, 'up and down')):
    cum = np.cumsum(filled(mo, axis))
    truth = np.interp(pic + (lag + 8) * 1000, pic, cum) - cum
    err = pd[:, axis - 1] - truth
    line = '%-11s all: needed %.2f left %.2f (%.0f %% removed)' % (name, rms(truth[half]), rms(err[half]), 100 * (1 - rms(err[half]) / max(1e-9, rms(truth[half]))))
    for a, b, what in ((0, 0.2, 'first 200 ms after the stick changed'), (0.2, 0.45, '200-450 ms'), (0.45, 9, 'later')):
        m = half & (since >= a) & (since < b)
        line += ' | %s: needed %.2f left %.2f' % (what, rms(truth[m]), rms(err[m]))
    print(line)
