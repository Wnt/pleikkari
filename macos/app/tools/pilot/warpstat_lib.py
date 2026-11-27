import numpy as np
def score(mo, a, b):
    mo = mo[(mo[:, 1] >= a) & (mo[:, 1] <= b)]
    pic, valid = mo[:, 1], mo[:, 4]
    lag = float(np.median(mo[:, 7]))
    dt = (mo[:, 1] - mo[:, 0]) / 1e6
    ok = valid > 0
    v = mo[:, 2] / np.maximum(dt, 1e-6)
    if ok.sum() >= 2:
        v = np.interp(pic, pic[ok], v[ok])
    cum = np.cumsum(v * dt)
    truth = np.interp(pic + (lag + 8) * 1000, pic, cum) - cum
    rms = lambda x: float(np.sqrt((x ** 2).mean()) * 100)
    return 'measured %.0f %%, explained p50 %.2f, to hide %.2f %%, left %.2f %% (%.0f %% removed)' % (100 * valid.mean(), np.median(mo[:, 10]), rms(truth), rms(mo[:, 5] - truth), 100 * (1 - rms(mo[:, 5] - truth) / max(1e-9, rms(truth))))
