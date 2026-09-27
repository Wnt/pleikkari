# PLE-800 device stimulus: press R3 (keycode 107, the app's default R3 mapping) N times,
# each held 80 ms so the down and the up land in different controller-state flushes.
# R3 does nothing in the PS5 menus; no stick moves, so the console's focus stays put.
n=${1:-100}
log -t PLE800 "stim start n=$n"
i=0
while [ $i -lt $n ]; do
	input keyevent --duration 80 107
	i=$((i+1))
done
log -t PLE800 "stim done n=$n"
