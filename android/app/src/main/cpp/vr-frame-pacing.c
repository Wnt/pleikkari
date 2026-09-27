// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "vr-frame-pacing.h"

#include <stdlib.h>
#include <string.h>

static void window_reset(PleikkariVrPacingWindow *window)
{
	memset(window, 0, sizeof(*window));
	window->wait_min_ns = INT64_MAX;
	window->ahead_min_ns = INT64_MAX;
	window->lead_min_ns = INT64_MAX;
}

void pleikkari_vr_pacing_config_default(PleikkariVrPacingConfig *config, float refresh_hz)
{
	memset(config, 0, sizeof(*config));
	config->mode = PLEIKKARI_VR_PACING_VRAPI;
	config->period_ns = refresh_hz > 0.0f ? (int64_t)(1e9 / refresh_hz + 0.5) : 1000000000LL / 72;
	config->budget_ns = PLEIKKARI_VR_PACING_DEFAULT_BUDGET_NS;
}

static int64_t parse_us(const char *text, const char **end)
{
	char *stop = NULL;
	long long us = strtoll(text, &stop, 10);
	if(end)
		*end = stop;
	return us > 0 ? (int64_t)us * 1000 : 0;
}

int pleikkari_vr_pacing_config_parse(PleikkariVrPacingConfig *config, const char *spec)
{
	int read = 0;
	if(!spec)
		return 0;
	char item[256];
	const char *cursor = spec;
	while(*cursor)
	{
		size_t length = strcspn(cursor, ",");
		if(length >= sizeof(item))
			length = sizeof(item) - 1;
		memcpy(item, cursor, length);
		item[length] = '\0';
		cursor += strcspn(cursor, ",");
		if(*cursor == ',')
			cursor++;
		const char *value = strchr(item, '=');
		if(value)
			value++;
		if(strcmp(item, "trace") == 0)
			config->trace = true;
		else if(strcmp(item, "late") == 0)
			config->mode = PLEIKKARI_VR_PACING_LATE;
		else if(strncmp(item, "sleep=", 6) == 0)
			config->sleep_ns = parse_us(value, NULL);
		else if(strncmp(item, "budget=", 7) == 0)
		{
			const int64_t budget = parse_us(value, NULL);
			if(budget <= 0)
				continue;
			config->budget_ns = budget;
		}
		else if(strncmp(item, "sweep=", 6) == 0)
		{
			// STEP_US/FRAMES/MAX_US
			const char *end;
			config->sweep_step_ns = parse_us(value, &end);
			config->sweep_frames = *end == '/' ? (uint32_t)strtoul(end + 1, (char **)&end, 10) : 0;
			config->sweep_max_ns = *end == '/' ? parse_us(end + 1, NULL) : 0;
			if(!config->sweep_frames || !config->sweep_step_ns)
				continue;
		}
		else if(strncmp(item, "stall=", 6) == 0)
		{
			// FRAME:US;FRAME:US
			const char *p = value;
			while(p && *p && config->stalls < PLEIKKARI_VR_PACING_MAX_STALLS)
			{
				char *end = NULL;
				unsigned long long frame = strtoull(p, &end, 10);
				if(!end || *end != ':')
					break;
				const char *after;
				int64_t ns = parse_us(end + 1, &after);
				if(ns > 0)
				{
					config->stall_frame[config->stalls] = frame;
					config->stall_ns[config->stalls] = ns;
					config->stalls++;
				}
				p = *after == ';' ? after + 1 : NULL;
			}
		}
		else
			continue;
		read++;
	}
	return read;
}

void pleikkari_vr_pacing_init(PleikkariVrPacing *pacing, const PleikkariVrPacingConfig *config)
{
	memset(pacing, 0, sizeof(*pacing));
	pacing->config = *config;
	window_reset(&pacing->window);
}

int64_t pleikkari_vr_pacing_wake_ns(PleikkariVrPacing *pacing, int64_t now_ns)
{
	const PleikkariVrPacingConfig *config = &pacing->config;
	const uint64_t frame = pacing->frames;
	int64_t wake = 0;
	// The late start keys on the last release only when VrApi made it: after a late frame the
	// next one starts at once, VrApi throttles it again, and the one after that is late-started.
	if(config->mode == PLEIKKARI_VR_PACING_LATE && pacing->return_ns > 0 && pacing->throttled)
	{
		const int64_t release = pacing->return_ns + config->period_ns;
		const int64_t start = release - config->budget_ns;
		if(start > now_ns && start < release)
			wake = start;
	}
	if(config->sweep_frames && frame > 0 && frame % config->sweep_frames == 0)
	{
		pacing->sweep_sleep_ns += config->sweep_step_ns;
		if(pacing->sweep_sleep_ns > config->sweep_max_ns)
			pacing->sweep_sleep_ns = config->sweep_max_ns;
	}
	int64_t extra = config->sleep_ns + pacing->sweep_sleep_ns;
	for(uint32_t i = 0; i < config->stalls; i++)
		if(config->stall_frame[i] == frame)
			extra += config->stall_ns[i];
	if(extra > 0)
		wake = (wake ? wake : now_ns) + extra;
	return wake;
}

void pleikkari_vr_pacing_frame(PleikkariVrPacing *pacing, int64_t start_ns, int64_t slept_ns,
		int64_t submit_ns, int64_t returned_ns, int64_t predicted_ns)
{
	PleikkariVrPacingWindow *window = &pacing->window;
	const int64_t work = submit_ns - start_ns;
	const int64_t wait = returned_ns - submit_ns;
	const int64_t ahead = predicted_ns - submit_ns;
	const int64_t lead = predicted_ns - returned_ns;
	const bool throttled = wait >= PLEIKKARI_VR_PACING_THROTTLED_NS;
	window->frames++;
	if(throttled)
		window->throttled++;
	else
		window->late++;
	window->slept_sum_ns += slept_ns;
	if(slept_ns > window->slept_max_ns)
		window->slept_max_ns = slept_ns;
	window->work_sum_ns += work;
	if(work > window->work_max_ns)
		window->work_max_ns = work;
	if(wait < window->wait_min_ns)
		window->wait_min_ns = wait;
	if(wait > window->wait_max_ns)
		window->wait_max_ns = wait;
	window->ahead_sum_ns += ahead;
	if(ahead < window->ahead_min_ns)
		window->ahead_min_ns = ahead;
	if(ahead > window->ahead_max_ns)
		window->ahead_max_ns = ahead;
	if(lead < window->lead_min_ns)
		window->lead_min_ns = lead;
	if(lead > window->lead_max_ns)
		window->lead_max_ns = lead;
	window->start_to_photon_sum_ns += predicted_ns - start_ns;
	pacing->return_ns = returned_ns;
	pacing->throttled = throttled;
	pacing->frames++;
}

void pleikkari_vr_pacing_take_window(PleikkariVrPacing *pacing, PleikkariVrPacingWindow *out)
{
	*out = pacing->window;
	if(!out->frames)
	{
		out->wait_min_ns = 0;
		out->ahead_min_ns = 0;
		out->lead_min_ns = 0;
	}
	window_reset(&pacing->window);
}

const char *pleikkari_vr_pacing_mode_name(PleikkariVrPacingMode mode)
{
	switch(mode)
	{
		case PLEIKKARI_VR_PACING_LATE: return "late start";
		default: return "VrApi release";
	}
}
