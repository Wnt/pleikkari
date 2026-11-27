"""The whole errand, by what the picture shows at each step: find the moon, enter inspect
mode, bring the moon to the middle, and (with N > 0) measure the input lag on it N times.

usage: moon_inspect.py SOCKET OUTDIR [N]

Every move is a short push of the stick followed by a look. The steps:
  1. third person: up until the moon shows (even half of it, behind the character), else round;
  2. inspect mode, read off the game's own prompts (pilot.enter_inspect): the view is then
     the character's own, and nothing stands before the moon;
  3. if the moon is not in that view, short pushes round where it was;
  4. to the middle; 5. the lag: a push, and the first picture in which the moon has moved.
"""
import random
import sys
import time

import cv2
import numpy as np

import scene
from pilot import Game, enter_inspect

sock, out = sys.argv[1], sys.argv[2]
count = int(sys.argv[3]) if len(sys.argv) > 3 else 0
g = Game(sock)
t0 = time.time()
log = open(out + "/moon.log", "a")
now_us = lambda: time.clock_gettime_ns(time.CLOCK_UPTIME_RAW) / 1000.0


def say(*a):
    line = "%6.2f s  " % (time.time() - t0) + " ".join(str(x) for x in a)
    print(line, flush=True)
    log.write(line + "\n")
    log.flush()


def save(pic, name, moon=None):
    big = cv2.cvtColor(cv2.resize(pic, (960, 540), interpolation=cv2.INTER_NEAREST), cv2.COLOR_GRAY2BGR)
    if moon:
        cv2.circle(big, (int(moon["x"] * 960), int(moon["y"] * 540)), int(moon["r"] * 960 * 2.5) + 6, (0, 255, 0), 1)
    cv2.line(big, (470, 270), (490, 270), (0, 0, 255), 1)
    cv2.line(big, (480, 260), (480, 280), (0, 0, 255), 1)
    cv2.imwrite(out + "/" + name, big)


def look(seconds=0.0, stop_on_moon=False, partial=True):
    end = time.time() + seconds
    while True:
        pic, t = g.frame()
        moon = scene.find_moon(pic, partial=partial)
        if (moon and stop_on_moon) or time.time() >= end:
            return pic, moon


def push(rx, ry, seconds, stop_on_moon=True):
    g.set(rx=rx, ry=ry)
    look(seconds, stop_on_moon=stop_on_moon)
    g.set(rx=0, ry=0)
    return look(0.45)


def where(m):
    return "at %.3f %.3f radius %.4f fill %.2f" % (m["x"], m["y"], m["r"], m["fill"]) if m else "not seen"


def centre(moon, tx=0.5, ty=0.5, seconds=14.0):
    """The moon to (tx, ty), as a player does it: the stick held gently towards it, less the
    nearer it is, every picture looked at; let go when it has been there for a few pictures."""
    began, there, gone, last = time.time(), 0, 0, moon
    try:
        while time.time() - began < seconds:
            pic, _ = g.frame()
            m = scene.find_moon(pic, partial=True)
            if not m:
                gone += 1
                if gone == 4:
                    g.set(rx=0, ry=0)   # out of sight: stop, and wait for the picture to say more
                if gone > 45:
                    save(pic, "lost.png")
                    return None
                continue
            gone, last = 0, m
            ex, ey = m["x"] - tx, m["y"] - ty
            if abs(ex) < 0.025 and abs(ey) < 0.035:
                there += 1
                g.set(rx=0, ry=0)
                if there >= 8:
                    return m
                continue
            there = 0
            amount = lambda e, tol: 0.0 if abs(e) < tol else (1 if e > 0 else -1) * min(0.75, 0.42 + 1.3 * abs(e))
            g.set(rx=amount(ex, 0.025), ry=amount(ey, 0.035))
        return last if there else None
    finally:
        g.set(rx=0, ry=0)


def horizon():
    """The horizon over the last few pictures: (kind, share, the last picture)."""
    kinds, shares = [], []
    for i in range(5):
        pic, _ = g.frame()
        k, v = scene.horizon(pic)
        kinds.append(k)
        shares.append(v)
    kind = max(set(kinds), key=kinds.count)
    return kind, float(np.median([v for k, v in zip(kinds, shares) if k == kind])), pic


def sweep(seconds=15.0):
    """One steady turn all the way round, every picture looked at, until the moon comes in.
    Says what the brightest round thing was if it was not the moon."""
    began, seen, moon, best = time.time(), 0, None, None
    g.set(rx=0.75, ry=0)
    while time.time() - began < seconds:
        pic, _ = g.frame()
        moon = scene.find_moon(pic)
        seen = seen + 1 if moon else 0
        if seen >= 2:
            break
        for c in scene.candidates(pic):
            if c["r"] > 0.006 and (best is None or c["peak"] - c["around"] > best["peak"] - best["around"]):
                best = c
    g.set(rx=0, ry=0)
    pic, moon = look(0.5)
    say("  turned for %.1f s: moon %s%s" % (time.time() - began, where(moon), "" if moon or not best else
        "; brightest other thing: radius %.4f fill %.2f aspect %.2f peak %.0f around %.0f at height %.2f" % (
            best["r"], best["fill"], best["aspect"], best["peak"], best["around"], best["y"])))
    return pic, moon


def scan():
    """As the user does it: the horizon first, then a little above it, then one turn round.
    The horizon is found from a height that cannot be mistaken: the camera as far up as it
    goes (bare sky), then down until something that is not sky comes into the lower part of
    the picture. That leaves the horizon low in the picture and sky above it, which is
    where the moon is."""
    g.set(rx=0, ry=-0.9)
    pic, moon = look(2.5, stop_on_moon=True, partial=False)
    g.set(rx=0, ry=0)
    if moon:
        return pic, moon
    began, hits, score = time.time(), 0, 0.0
    g.set(rx=0, ry=0.55)
    while time.time() - began < 8.0:
        pic, _ = g.frame()
        moon = scene.find_moon(pic)
        yes, score = scene.ground_in_strip(pic)
        hits = hits + 1 if yes else 0
        if moon or hits >= 3:
            break
    g.set(rx=0, ry=0)
    say("  down from the top for %.1f s: %s" % (time.time() - began, "the horizon is in the lower part of the picture (%.1f)" % score if hits >= 3 else "moon" if moon else "no horizon seen"))
    pic, moon = look(0.5, partial=False)
    save(pic, "1-height.png", moon)
    for what, nudge in (("at the horizon", 0), ("a little higher", -0.6), ("higher still", -0.6), ("lower", 0.6)):
        if moon:
            break
        if nudge:
            g.set(rx=0, ry=nudge)
            pic, moon = look(0.3 if nudge < 0 else 0.9, stop_on_moon=True, partial=False)
            g.set(rx=0, ry=0)
        if not moon:
            say("  turning round, %s" % what)
            pic, moon = sweep()
    return pic, moon


def search(pattern, what):
    pic, moon = look(0.3)
    for i, (rx, ry, seconds) in enumerate(pattern):
        if moon:
            break
        pic, moon = push(rx, ry, seconds)
        say("  %s %d (%+.1f %+.1f): moon %s" % (what, i, rx, ry, where(moon)))
    return pic, moon


try:
    pic, moon = look(0.3)
    state = scene.prompt_state(pic)[0]
    say("first look: prompts '%s', brightness %.0f, moon %s" % (state, pic.mean(), where(moon)))
    save(pic, "0-start.png", moon)
    if state != "inspecting":
        if not moon:
            say("step 1: looking for the moon")
            pic, moon = scan()
            save(pic, "1-found.png" if moon else "1-notfound.png", moon)
        if not moon:
            raise SystemExit("no moon found")
        # Inspect mode looks where the middle of this picture looks: the moon goes there
        # first, a little above the middle, clear of the character's head.
        say("step 1b: the moon towards the middle before the view changes (%s)" % where(moon))
        moon = centre(moon, 0.5, 0.33) or moon
        say("step 2: the moon is in the picture (%s): entering inspect mode" % where(moon))
        if not enter_inspect(g, say):
            raise SystemExit("could not enter inspect mode")
    else:
        g.set(l2=1.0)
    pic, moon = look(0.6)
    save(pic, "2-inspect.png", moon)
    say("in inspect mode: moon %s" % where(moon))
    if not moon:
        say("step 3: not in this view: short pushes round")
        box = [(0, -0.7, 0.2), (-0.7, 0, 0.2), (0, 0.7, 0.2), (0, 0.7, 0.2), (0.7, 0, 0.2), (0.7, 0, 0.2), (0, -0.7, 0.2), (0, -0.7, 0.2),
               (0, -0.7, 0.2), (-0.7, 0, 0.2), (-0.7, 0, 0.2), (-0.7, 0, 0.2)]
        pic, moon = search(box + [(0.8, 0, 0.3)] * 30, "round")
        if not moon:
            save(pic, "3-notfound.png")
            raise SystemExit("the moon is not to be found in inspect mode")
    say("step 4: to the middle")
    moon = centre(moon)
    if not moon:
        raise SystemExit("lost the moon")
    pic, moon = look(0.4, partial=False)
    save(pic, "4-centred.png", moon)
    say("the moon is in the middle, whole: %s; prompts '%s'" % (where(moon), scene.prompt_state(pic)[0]))
    ok = bool(moon) and scene.prompt_state(pic)[0] == "inspecting"
    say("RESULT: %s" % ("moon centred in inspect mode" if ok else "NOT there"))

    lags, direction = [], 1
    for n in range(count if ok else 0):
        xs, end = [], time.time() + 0.6 + random.random() * 0.5
        while (time.time() < end or len(xs) < 6) and time.time() < end + 3:
            pic, t = g.frame(newest=False)
            m = scene.find_moon(pic)
            if m:
                xs.append(m["x"])
        if len(xs) < 6:
            say("trial", n, "no moon: bringing it back")
            pic, m = look(0.3)
            if m:
                centre(m)
            continue
        base, noise = float(np.median(xs[-8:])), float(np.std(xs[-8:]))
        if abs(base - 0.5) > 0.15:
            direction = 1 if base > 0.5 else -1
        limit = max(0.003, 5 * noise)
        pushed = now_us()
        g.set(rx=0.8 * direction)
        lag, released, late = None, False, 0
        while now_us() - pushed < 700e3:
            pic, t = g.frame(newest=False)
            late = max(late, (now_us() - t) / 1000.0)
            if not released and now_us() - pushed > 180e3:
                g.set(rx=0)
                released = True
            m = scene.find_moon(pic)
            if lag is None and t > pushed and m and abs(m["x"] - base) > limit:
                lag = (t - pushed) / 1000.0
        g.set(rx=0)
        if lag is None:
            say("trial", n, "the moon did not move")
        else:
            lags.append(lag)
            say("trial %d: %+d from %.3f: moved after %.0f ms (pictures read at most %.0f ms after they arrived)" % (n, direction, base, lag, late))
        direction = -direction
    if lags:
        a = np.array(lags)
        say("input lag on the moon in inspect mode, stick to picture arrived: n=%d min %.0f p10 %.0f median %.0f mean %.0f p90 %.0f max %.0f ms"
            % (len(a), a.min(), np.percentile(a, 10), np.median(a), a.mean(), np.percentile(a, 90), a.max()))
finally:
    g.set(rx=0, ry=0, buttons=0)
    time.sleep(0.1)
    if count:
        g.set(l2=0)
        time.sleep(0.1)
        g.release()
    # Without a lag run the program leaves L2 held, so the game stays in inspect mode on the
    # moon; the next program to connect, or `pilot.py SOCKET release`, takes over.
