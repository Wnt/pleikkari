# PLE-829 device stimulus (patterned on PLE-800's): under a Perfetto trace of the app's view and
# input slices, press R3 (keycode 107, the app's default R3 mapping) N times. Each press is held 80 ms
# so the down and the up land in different controller states. R3 does nothing in the PS5 menus, and
# no stick moves, so the console's focus stays put. A pause of 0-0.3 s between presses keeps them
# from locking to the display's frames. The trace stops right after the presses.
#   sh stim.sh <presses> <trace file> <config in /data/misc/perfetto-configs>
# (A backgrounded perfetto writes nothing on SIGINT, and one that reads a config from elsewhere
# gets permission denied: hence --detach / --attach --stop, and trace.cfg's write_into_file.)
n=${1:-40}
trace=${2:-/data/misc/perfetto-traces/ple829.pftrace}
cfg=${3:-/data/misc/perfetto-configs/ple829.cfg}
perfetto --txt -c "$cfg" --detach=ple829 -o "$trace" 2>&1 | tail -1
sleep 1
log -t PLE829 "stim start n=$n"
i=0
while [ $i -lt $n ]; do
	input keyevent --duration 80 107
	sleep 0.$(( (i * 7) % 4 ))
	i=$((i+1))
done
log -t PLE829 "stim done n=$n"
perfetto --attach=ple829 --stop 2>&1 | tail -1
