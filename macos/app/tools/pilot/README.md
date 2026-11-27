# pilot: the game played by a program that looks first

Tools for testing the native app against a real console without a hand on the controller.
They talk to a running `Pleikkari stream ... --control SOCKET [--record-input PREFIX]`:
the app sends every new picture of the game (480 x 270, grey) down the socket and takes
the controller from it.

| file | what it does |
|---|---|
| `pilot.py` | the socket: pictures in, controller out; `look`, `level`, `release`, `turn` from the command line; entering the game's inspect mode by reading its button prompts |
| `scene.py` | what is in a picture (OpenCV): the moon, the button prompts, whether ground has come into view |
| `moon_inspect.py` | find the moon, enter inspect mode, centre it, measure the input lag on it |
| `modes_test.py` | the same stick work in free look, armed, aiming and inspect mode, with the camera warp scored in each |
| `thumb.py`, `pans.py` | right-stick work the way a thumb does it, and plain pans |
| `warpstat.py RUNDIR [s]` | what the live camera warp did in the last seconds of a recording session |
| `offline.py RUNDIR [max]` | replay a session's stick and measured motion through `plk-warp-eval` and break the error down |
| `capture_pan.sh OUTDIR` | pan while capturing the app's own window (by window id, so nothing on top of it matters) |
| `restart.sh RUN [options]` | install the current build and start a recording, controllable HDR stream |

They were written on one Mac in one night (2026-11-27): the workspace is taken from
`PLK_WORKSPACE` (default `~/Documents/pleikkari-android`), and `restart.sh` names the
console and its address; change those first. Python with numpy and OpenCV (`opencv-python-headless`) is needed.

Every move a program makes is a short push followed by a look at the picture; nothing is
steered blind.
