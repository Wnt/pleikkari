"""Right-stick work the way a thumb does it: smooth, never a jump. thumb.py SOCKET SECONDS [l2]
Eased pushes of varied size and length, slow drifts, circles, small corrections round a point,
all sent a hundred times a second. Seeded: the same every run."""
import math, random, sys, time
from pilot import Game, level
g = Game(sys.argv[1])
total = float(sys.argv[2])
l2 = 1.0 if len(sys.argv) > 3 and sys.argv[3] == "l2" else 0.0
rnd = random.Random(11)
ease = lambda u: u * u * (3 - 2 * u)
def run(f, seconds):
    t0 = time.time()
    while True:
        t = time.time() - t0
        if t >= seconds:
            break
        x, y = f(t)
        g.set(rx=x, ry=y, l2=l2)
        time.sleep(0.01)
try:
    g.set(l2=l2)
    time.sleep(0.8)
    began = time.time()
    levelled = time.time()
    level(g)
    while time.time() - began < total:
        if time.time() - levelled > 1e9:    # up-and-down work adds up: back to level now and then
            level(g)
            g.set(l2=l2)
            time.sleep(0.5)
            levelled = time.time()
        kind = rnd.choice(["push", "push", "push", "drift", "circle", "correct", "flick"])
        if kind == "push":      # out over 60-150 ms, held, back over 50-120 ms
            a = rnd.choice([0.45, 0.6, 0.75, 0.9, 1.0]) * rnd.choice([-1, 1]); b = rnd.uniform(-0.25, 0.25)
            up, hold, down = rnd.uniform(0.06, 0.15), rnd.uniform(0.15, 0.9), rnd.uniform(0.05, 0.12)
            run(lambda t: ((a * ease(t / up), b * ease(t / up)) if t < up else (a, b) if t < up + hold else (a * (1 - ease((t - up - hold) / down)), b * (1 - ease((t - up - hold) / down)))), up + hold + down)
        elif kind == "drift":   # a slow sweep that changes its mind
            a, w = rnd.uniform(0.5, 0.85), rnd.uniform(1.5, 3.5)
            run(lambda t: (a * math.sin(w * t), 0.3 * a * math.sin(1.7 * w * t)), rnd.uniform(1.5, 3.0))
        elif kind == "circle":
            a, w = rnd.uniform(0.5, 0.8), rnd.uniform(2.0, 5.0)
            run(lambda t: (a * math.cos(w * t) * ease(min(1, t * 4)), a * 0.6 * math.sin(w * t) * ease(min(1, t * 4))), rnd.uniform(1.2, 2.5))
        elif kind == "correct": # small corrections, as when lining up on something
            for i in range(rnd.randint(3, 6)):
                a = rnd.uniform(0.4, 0.65) * rnd.choice([-1, 1]); d = rnd.uniform(0.08, 0.25)
                run(lambda t: (a * math.sin(math.pi * t / d), 0), d)
                run(lambda t: (0, 0), rnd.uniform(0.1, 0.3))
        else:                   # a flick: full over and back in 120-220 ms
            a = rnd.choice([-1, 1]); d = rnd.uniform(0.12, 0.22)
            run(lambda t: (a * math.sin(math.pi * t / d), 0), d)
        run(lambda t: (0, 0), rnd.uniform(0.2, 0.6))
finally:
    g.set(rx=0, ry=0, l2=0)
    time.sleep(0.2)
    g.release()
