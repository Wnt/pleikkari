// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// plk-gamepad-probe: what the app's gamepad support sees, without a window, a stream or a
// console. Lists the controllers GameController offers (the DualSense is the HID driver's
// and is skipped), starts their rumble engines, prints every mapped change, and with
// --pulse runs the motors: heavy, light, then the haptics-stream path.
//
//   plk-gamepad-probe [--seconds N] [--pulse] [--quiet]

#include "Gamepads.h"

#import <Foundation/Foundation.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace plk::app;

int main(int argc, char **argv)
{
	double seconds = 5;
	bool pulse = false, quiet = false;
	for(int i = 1; i < argc; i++)
	{
		if(!strcmp(argv[i], "--seconds") && i + 1 < argc)
			seconds = atof(argv[++i]);
		else if(!strcmp(argv[i], "--pulse"))
			pulse = true;
		else if(!strcmp(argv[i], "--quiet"))
			quiet = true;
		else
		{
			fprintf(stderr, "usage: plk-gamepad-probe [--seconds N] [--pulse] [--quiet]\n");
			return 2;
		}
	}
	@autoreleasepool
	{
		GamepadHub hub;
		GamepadHub::Config config;
		GamepadHub *h = &hub;
		hub.start(config,
				[quiet](const ChiakiControllerState &s) {
					if(!quiet)
						printf("state buttons=%08x l2=%3u r2=%3u left=(%6d,%6d) right=(%6d,%6d)\n", s.buttons,
								s.l2_state, s.r2_state, s.left_x, s.left_y, s.right_x, s.right_y);
				},
				[](char level, const std::string &message) { printf("[%c] %s\n", level, message.c_str()); });
		if(pulse)
		{
			auto at = [](double s, dispatch_block_t block) {
				dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(s * NSEC_PER_SEC)), dispatch_get_main_queue(), block);
			};
			at(1.0, ^{
				printf("pulse: heavy motor (left handle) at 60 %%\n");
				h->setSessionActive(true);
				h->onRumble(153, 0);
			});
			at(1.6, ^{
				printf("pulse: light motor (right handle) at 60 %%\n");
				h->onRumble(0, 153);
			});
			at(2.2, ^{
				printf("pulse: off\n");
				h->onRumble(0, 0);
			});
			// 0.5 s of a haptics stream at a quarter of full scale: both motors at 50 %.
			for(int i = 0; i < 50; i++)
				at(2.8 + i * 0.01, ^{
					int16_t pcm[60];
					for(int k = 0; k < 30; k++)
						pcm[2 * k] = pcm[2 * k + 1] = (int16_t)((k & 1) ? 8192 : -8192);
					if(i == 0)
						printf("pulse: haptics stream, 0.5 s\n");
					h->onHapticsFrame((const uint8_t *)pcm, sizeof(pcm));
				});
			at(3.6, ^{
				printf("pulse: done\n");
				h->setSessionActive(false);
			});
			if(seconds < 4.2)
				seconds = 4.2;
		}
		[[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
		printf("%s\n", hub.statsLine().c_str());
		hub.stop();
	}
	return 0;
}
