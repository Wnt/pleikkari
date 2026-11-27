// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// plk-warp-eval: a recording of `Pleikkari stream ... --stick-sweep PREFIX` played through the
// camera warp again, with no console: PREFIX-stick.csv and PREFIX-motion.csv in, and for
// every picture what the warp would show it moved by, a refresh after it arrived, out
// (picture_us,warp_x,warp_y,delay_ms,gain_x,gain_y,explained). A change to the warp's
// learning is scored on the same play as the one before it.
//
// usage: plk-warp-eval PREFIX > predictions.csv

#include "CameraWarp.h"

#include <cstdlib>

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char **argv)
{
	if(argc < 2)
	{
		fprintf(stderr, "usage: plk-warp-eval PREFIX [--aiming] [PREFIX [--aiming]...]\n"
				"  reads PREFIX-stick.csv and PREFIX-motion.csv; several recordings are played one after\n"
				"  the other, as one sitting. --aiming: L2 was held throughout that recording (one made\n"
				"  before the stick file had a column for it)\n");
		return 2;
	}
	plk::app::CameraWarp warp;
	if(getenv("PLK_WARP_MAX_SHIFT"))
		warp.setMaxShift(atof(getenv("PLK_WARP_MAX_SHIFT")));
	printf("picture_us,warp_x,warp_y,delay_ms,gain_x,gain_y,explained\n");
	double shift = 0, end_of_last = 0;
	for(int arg = 1; arg < argc; arg++)
	{
	const std::string prefix = argv[arg];
	const bool all_aiming = arg + 1 < argc && std::string(argv[arg + 1]) == "--aiming";
	if(all_aiming)
		arg++;
	struct Stick { double t; float x, y; int aiming; };
	struct Motion { double previous, picture; float dx, dy; int valid; };
	std::vector<Stick> stick;
	std::vector<Motion> motion;
	char line[512];
	FILE *f = fopen((prefix + "-stick.csv").c_str(), "r");
	if(!f)
	{
		fprintf(stderr, "cannot read %s-stick.csv\n", prefix.c_str());
		return 1;
	}
	while(fgets(line, sizeof(line), f))
	{
		Stick s;
		s.aiming = 0;
		if(sscanf(line, "%lf,%f,%f,%d", &s.t, &s.x, &s.y, &s.aiming) >= 3)
		{
			if(all_aiming)
				s.aiming = 1;
			stick.push_back(s);
		}
	}
	fclose(f);
	f = fopen((prefix + "-motion.csv").c_str(), "r");
	if(!f)
	{
		fprintf(stderr, "cannot read %s-motion.csv\n", prefix.c_str());
		return 1;
	}
	while(fgets(line, sizeof(line), f))
	{
		Motion m;
		if(sscanf(line, "%lf,%lf,%f,%f,%d", &m.previous, &m.picture, &m.dx, &m.dy, &m.valid) == 5)
			motion.push_back(m);
	}
	fclose(f);

	// One recording after the other in time, two seconds apart.
	if(!motion.empty())
	{
		shift = end_of_last > 0 ? end_of_last + 2e6 - motion.front().previous : 0;
		end_of_last = motion.back().picture + shift;
	}
	size_t at = 0;
	for(const Motion &m : motion)
	{
		// Everything the stick did up to the moment the picture arrived, then the picture.
		while(at < stick.size() && stick[at].t <= m.picture)
		{
			warp.onStick(stick[at].t + shift, stick[at].x, stick[at].y, stick[at].aiming);
			at++;
		}
		warp.onMotion(m.previous + shift, m.picture + shift, m.dx, m.dy, m.valid != 0);
		float x = 0, y = 0;
		warp.predict(m.picture + shift, m.picture + shift + 8000.0, &x, &y);
		const plk::app::CameraWarp::State st = warp.state();
		printf("%.0f,%.6f,%.6f,%.0f,%.4f,%.4f,%.3f\n", m.picture, x, y, st.delay_ms, st.gain_x, st.gain_y, st.explained);
	}
	}
	fputs(warp.describe().c_str(), stderr);
	return 0;
}
