"""The game through Pleikkari's --control socket: its pictures in, the controller out."""
import socket
import struct
import sys
import time

import cv2
import numpy as np

import scene

R3 = 1 << 11
L2_BUTTON = 1 << 16


class Game:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(path)
        self.buf = b""
        self.pad = dict(buttons=0, l2=0, r2=0, lx=0, ly=0, rx=0, ry=0)
        self.holding = False

    def _read(self, n):
        while len(self.buf) < n:
            d = self.s.recv(1 << 18)
            if not d:
                raise EOFError("the app has gone")
            self.buf += d
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def frame(self, newest=True):
        """The next picture: (grey array, arrival time in us). newest: skip what has queued up."""
        while True:
            h = self._read(16)
            assert h[:4] == b"PLKF", h[:4]
            w, hh = struct.unpack_from("<HH", h, 4)
            t = struct.unpack_from("<d", h, 8)[0]
            g = np.frombuffer(self._read(w * hh), np.uint8).reshape(hh, w)
            if not newest:
                return g, t
            self.s.setblocking(False)
            try:
                more = self.s.recv(1 << 18)
                self.buf += more
            except BlockingIOError:
                more = b""
            self.s.setblocking(True)
            if len(self.buf) < 16 + w * hh:
                return g, t

    def set(self, **kw):
        """Change the controller: rx, ry, lx, ly as -1...1, l2/r2 as 0...1, buttons as a mask."""
        self.pad.update(kw)
        p = self.pad
        buttons = int(p["buttons"]) | (L2_BUTTON if p["l2"] > 0.4 else 0)
        st = lambda v: int(max(-1.0, min(1.0, v)) * 32767)
        self.s.sendall(("%d %d %d %d %d %d %d\n" % (buttons, int(p["l2"] * 255), int(p["r2"] * 255), st(p["lx"]), st(p["ly"]),
                                                    st(p["rx"]), st(p["ry"]))).encode())
        self.holding = True

    def release(self):
        self.s.sendall(b"release\n")
        self.holding = False


def wait_state(g, wanted, seconds, say=None):
    """Watch the pictures until the prompt corner reads `wanted` in three pictures running.
    Returns (reached, the last picture, how long it took)."""
    began, run, pic = time.time(), 0, None
    while time.time() - began < seconds:
        pic, _ = g.frame()
        state = scene.prompt_state(pic)[0]
        run = run + 1 if state == wanted else 0
        if run >= 3:
            return True, pic, time.time() - began
    return False, pic, time.time() - began


def enter_inspect(g, say=print):
    """Into the game's inspect mode, by what the game itself shows: hold L2 until it offers
    "INSPECT R3", press R3 until it shows "ZOOM R2 / EXIT R3", then wait for the view to
    have faded back in. L2 stays held (letting it go leaves the mode). True when in."""
    pic, _ = g.frame()
    if scene.prompt_state(pic)[0] == "inspecting" and g.pad["l2"] > 0.5:
        return True
    g.set(l2=1.0, buttons=0, rx=0, ry=0)
    ok, pic, took = wait_state(g, "offered", 4.0)
    if not ok:
        # Already in, with L2 held by this program before?
        if scene.prompt_state(pic)[0] != "inspecting":
            say("inspect is not offered after %.1f s of L2: prompts %s" % (took, scene.prompt_state(pic)))
            return False
    else:
        say("L2 held: the game offers inspect after %.2f s" % took)
        for attempt, press in enumerate((0.15, 0.25, 0.4, 0.4)):
            g.set(buttons=R3)
            time.sleep(press)
            g.set(buttons=0)
            ok, pic, took = wait_state(g, "inspecting", 3.0)
            if ok:
                say("R3 pressed for %.2f s (attempt %d): in inspect mode after %.2f s" % (press, attempt + 1, took))
                break
            say("R3 pressed for %.2f s (attempt %d): no change, prompts %s" % (press, attempt + 1, scene.prompt_state(pic)))
        if not ok:
            return False
    # The view fades through black and comes back: wait until the picture's brightness has
    # stopped changing for half a second.
    last, steady, began = None, 0.0, time.time()
    while time.time() - began < 8.0:
        pic, _ = g.frame()
        mean = float(pic.mean())
        if last is not None and abs(mean - last) < 0.4 and mean > 3.0:
            steady += 1
        else:
            steady = 0
        last = mean
        if steady >= 15:
            break
    say("the view has settled %.1f s later (brightness %.0f)" % (time.time() - began, last))
    return scene.prompt_state(pic)[0] == "inspecting"


def leave_inspect(g, say=print):
    g.set(l2=0.0, buttons=0, rx=0, ry=0)
    ok, pic, took = wait_state(g, "none", 5.0)
    say("L2 let go: prompts gone after %.2f s" % took if ok else "L2 let go, but prompts still read %s" % (scene.prompt_state(pic),))
    return ok


def level(g, say=None):
    """The camera to a height that is known: the game puts it back behind the character, at
    its usual height, when the character walks on; from there a short push up brings the
    horizon to about the middle of the picture. (Reading the horizon off the picture did
    not hold up: clouds passed for ground, and dark sand for sky.)"""
    g.set(lx=0, ly=-1.0, rx=0, ry=0)
    time.sleep(1.1)
    g.set(lx=0, ly=0)
    time.sleep(1.8)
    g.set(ry=-0.8)
    time.sleep(0.32)
    g.set(ry=0)
    time.sleep(0.6)
    return True


if __name__ == "__main__":
    g = Game(sys.argv[1])
    if sys.argv[2] == "look":
        time.sleep(0.3)
        pic, t = g.frame()
        cv2.imwrite(sys.argv[3], cv2.resize(pic, (960, 540), interpolation=cv2.INTER_NEAREST))
        print("picture:", scene.describe(pic))
        print("moon:", scene.find_moon(pic))
        for c in scene.candidates(pic):
            print("  bright patch:", {k: round(v, 3) for k, v in c.items()})
    elif sys.argv[2] == "level":
        print(level(g, print))
        g.release()
    elif sys.argv[2] == "release":
        g.set(l2=0, buttons=0, rx=0, ry=0)
        time.sleep(0.2)
        g.release()
    elif sys.argv[2] == "turn":
        # Turn the camera away, to start an errand from somewhere else: turn RX RY SECONDS
        g.set(l2=0, buttons=0, rx=float(sys.argv[3]), ry=float(sys.argv[4]))
        time.sleep(float(sys.argv[5]))
        g.set(rx=0, ry=0)
        time.sleep(0.6)
        g.release()
