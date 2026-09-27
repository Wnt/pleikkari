// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "latency-trace.h"

#include <android/trace.h>

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>

static atomic_bool armed = false;

void android_chiaki_latency_trace_arm(bool value)
{
	atomic_store(&armed, value);
}

bool android_chiaki_latency_trace_armed(void)
{
	return atomic_load_explicit(&armed, memory_order_relaxed);
}

bool android_chiaki_latency_trace_on(void)
{
	return android_chiaki_latency_trace_armed() && ATrace_isEnabled();
}

void android_chiaki_latency_trace_mark(const char *fmt, ...)
{
	if(!android_chiaki_latency_trace_on())
		return;
	char name[128];
	va_list args;
	va_start(args, fmt);
	vsnprintf(name, sizeof(name), fmt, args);
	va_end(args);
	ATrace_beginSection(name);
	ATrace_endSection();
}

bool android_chiaki_latency_trace_begin(const char *name)
{
	if(!android_chiaki_latency_trace_on())
		return false;
	ATrace_beginSection(name);
	return true;
}

void android_chiaki_latency_trace_end(void)
{
	ATrace_endSection();
}
