"""What is in a picture of the game, for the automatic camera: the moon, if it is there.

find_moon(gray) takes an 8-bit grey picture of any size and returns None or a dict with the
disc's centre (0...1 each way), its radius (of the width), how round it is and how dark the
sky around it is. The moon is told from lamps, windows and the HUD by being a bright,
compact, round patch with dark sky all round it, and by not being where the HUD is.
"""
import cv2
import numpy as np

# Where the game draws over the picture (x0, y0, x1, y1 of the picture): the minimap, the
# button prompts, the console's notices.
HUD = ((0.0, 0.70, 0.20, 1.0), (0.80, 0.70, 1.0, 1.0), (0.38, 0.0, 1.0, 0.09), (0.75, 0.0, 1.0, 0.22))


def candidates(gray):
    h, w = gray.shape
    blur = cv2.GaussianBlur(gray, (0, 0), max(1.0, w / 960.0))
    # Bright against this picture's own brightest point outside the HUD, not against a set
    # level: after a fast turn the stream is soft for a moment and the moon with it (its
    # peak fell under 150 of 255 and it was "lost", 2026-11-27).
    sky = blur.copy()
    for a, b, c, d in HUD:
        sky[int(b * h):int(d * h), int(a * w):int(c * w)] = 0
    level = max(105.0, 0.6 * float(sky.max()))
    _, mask = cv2.threshold(sky, level, 255, cv2.THRESH_BINARY)
    n, labels, stats, cents = cv2.connectedComponentsWithStats(mask, connectivity=8)
    out = []
    for i in range(1, n):
        x, y, bw, bh, area = stats[i]
        cx, cy = cents[i][0] / w, cents[i][1] / h
        if any(a <= cx <= c and b <= cy <= d for a, b, c, d in HUD):
            continue
        r = np.sqrt(area / np.pi)
        if r < w * 0.004 or r > w * 0.08:
            continue
        # Round: as wide as high, and filling the circle of its area.
        fill = area / (np.pi * (max(bw, bh) / 2.0) ** 2)
        aspect = min(bw, bh) / max(bw, bh)
        # The sky round it: a ring from 1.6 to 3 radii out.
        yy, xx = np.ogrid[:h, :w]
        d2 = (xx - cents[i][0]) ** 2 + (yy - cents[i][1]) ** 2
        ring = (d2 > (1.6 * r) ** 2) & (d2 < (3.0 * r) ** 2)
        around = float(blur[ring].mean()) if ring.any() else 255.0
        out.append(dict(x=cx, y=cy, r=r / w, fill=float(fill), aspect=float(aspect), around=around,
                        peak=float(blur[labels == i].max())))
    return out


def find_moon(gray, partial=False):
    """partial: also a moon that is half behind something (the character's shoulder, the
    picture's edge): less round, and what is around it need not all be sky."""
    best = None
    for c in candidates(gray):
        if partial:
            if c["fill"] < 0.25 or c["aspect"] < 0.35 or c["peak"] - c["around"] < 70 or c["peak"] < 150:
                continue
        elif c["fill"] < 0.6 or c["aspect"] < 0.7 or c["peak"] - c["around"] < 90:
            continue
        # Not smaller than the moon ever is (0.014 of the width in the third person view,
        # 0.025 and more when inspecting): a marker the game hangs in the world is a third
        # of that, and was taken for the moon once.
        if c["r"] < 0.009:
            continue
        if best is None or c["r"] > best["r"]:
            best = c
    return best


def describe(gray):
    """A few words on the picture as a whole, for the log."""
    h, w = gray.shape
    return dict(mean=float(gray.mean()), upper=float(gray[: h // 3].mean()), lower=float(gray[2 * h // 3:].mean()),
                bright_share=float((gray > 150).mean()))


def prompt_state(gray):
    """Which button prompts the game shows in the lower right corner, read off the picture.

    'none': no prompt. 'offered': one line ("INSPECT R3": aiming, and inspect mode can be
    entered). 'inspecting': two lines ("ZOOM R2" above "EXIT R3": in inspect mode). Told by
    which of the two lines of white writing are there; the numbers are how much of each.
    """
    h, w = gray.shape
    x0, x1 = int(w * 0.885), int(w * 0.995)
    upper = gray[int(h * 0.893):int(h * 0.932), x0:x1]
    lower = gray[int(h * 0.942):int(h * 0.981), x0:x1]
    # White writing on the picture: clearly brighter than what it is written on, whatever
    # that is (dark sand at night, bright sand by day: a set level failed on the second).
    count = lambda band: int((band.astype(np.int16) > max(105, int(np.median(band)) + 45)).sum())
    u, l = count(upper), count(lower)
    state = "inspecting" if u >= 6 and l >= 6 else "offered" if l >= 6 else "none"
    return state, u, l


def horizon(gray):
    """Where the horizon is, read off the picture: (kind, share).

    'horizon': sky above, ground below; share is how much of the height, from the top, is sky.
    'ground':  the camera looks down: what is rough (trees, houses, surf) is at the top and
               smooth ground below it. share 0.
    'blank':   nothing to tell by: all sky, or all smooth ground or sea. share 1.
    Sky at night is smooth and most of what is on the ground is not. Looked at beside the
    character and clear of the HUD. (A first version called everything smooth "sky" and
    steered the camera down into the sand, 2026-11-27.)
    """
    h, w = gray.shape
    # Whatever the hour, the sky and what lies under it differ in brightness: darker than
    # sand at night, brighter than trees and sea at dawn. The horizon is where the picture's
    # rows change most, going down. (Told by roughness at first: at dusk the sand was as
    # smooth as the sky, and a horizon in the middle of the picture was read as at the bottom.)
    blur = cv2.GaussianBlur(gray, (0, 0), 2.0).astype(np.float32)
    cols = np.r_[int(w * 0.04):int(w * 0.30), int(w * 0.58):int(w * 0.87)]
    rows = blur[: int(h * 0.80)][:, cols]
    # each side by itself (trees on one, sea on the other), the stronger of the two
    best = ("blank", 1.0, 0.0)
    for part in (rows[:, : int(w * 0.26)], rows[:, int(w * 0.26):]):
        p = np.median(part, axis=1)
        p = np.convolve(np.pad(p, 4, mode="edge"), np.ones(9) / 9.0, mode="valid")
        step = np.abs(p[10:] - p[:-10])
        at = int(np.argmax(step))
        if step[at] > max(5.0, best[2]):
            best = ("horizon", float(at + 5) / h, float(step[at]))
    return best[0], best[1]


def sky_share(gray):
    """How much of the picture's height, from the top down, is sky: 0 (none) to 1 (all).

    Sky at night is smooth and what is on the ground (trees, sand, surf, houses) is not, so
    the horizon is the first row from the top where the picture turns rough and stays so.
    Looked at beside the character and clear of the HUD: the right part of the picture.
    """
    h, w = gray.shape
    band = cv2.GaussianBlur(gray[: int(h * 0.86), int(w * 0.58): int(w * 0.97)], (0, 0), 1.2).astype(np.float32)
    rough = np.abs(np.diff(band, axis=1)).mean(axis=1) + np.abs(np.diff(band, axis=0, prepend=band[:1])).mean(axis=1)
    rough = np.convolve(rough, np.ones(7) / 7.0, mode="same")
    level = max(0.9, float(np.percentile(rough, 10)) * 3.0)
    # The horizon: the highest row from which nearly everything down to the bottom is rough.
    # (The moon, a star or a marker in the sky is a few rough rows with smooth ones below.)
    is_rough = (rough > level).astype(np.float32)
    share_below = np.cumsum(is_rough[::-1])[::-1] / np.arange(len(is_rough), 0, -1)
    ok = np.where((share_below > 0.85) & (is_rough > 0))[0]
    if len(ok) and len(is_rough) - ok[0] >= 6:
        return float(ok[0]) / h
    return 1.0


def ground_in_strip(gray):
    """Coming down from bare sky: has something that is not sky come into the lower part of
    the picture? Compared with the sky at the top of the same picture, beside the character
    and clear of the HUD: rougher, or of another brightness. Returns (yes, how much)."""
    h, w = gray.shape
    blur = cv2.GaussianBlur(gray, (0, 0), 1.5).astype(np.float32)
    cols = np.r_[int(w * 0.22):int(w * 0.36), int(w * 0.62):int(w * 0.86)]
    sky = blur[int(h * 0.06):int(h * 0.34)][:, cols]
    low = blur[int(h * 0.70):int(h * 0.84)][:, cols]
    rough = lambda a: float(np.abs(np.diff(a, axis=1)).mean() + np.abs(np.diff(a, axis=0)).mean())
    score = max(rough(low) / max(0.25, rough(sky)) / 2.5, abs(float(np.median(low)) - float(np.median(sky))) / 9.0)
    return score > 1.0, score
