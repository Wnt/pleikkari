# PLE-823 device stimulus: N small left-stick sweeps, out to 15 % and back to centre, each over
# 60 ms, injected as a joystick (no touch on the screen). Android delivers the sweep's moves
# about once a frame and its final sample, the stick back at 0, right after the last move:
# inside the 8 ms state minimum. The pause lets that sample go out on its own before the next
# sweep. 15 % is well under the PS5 menus' stick threshold, so the console's focus stays put.
n=${1:-40}
log -t PLE823 "stim start n=$n"
i=0
while [ $i -lt $n ]; do
	input joystick swipe 0 0 0.15 0 60
	input joystick swipe 0.15 0 0 0 60
	sleep 0.5
	i=$((i+1))
done
log -t PLE823 "stim done n=$n"
