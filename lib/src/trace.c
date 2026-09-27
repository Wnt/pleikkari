// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <chiaki/trace.h>

#include <stddef.h>

static ChiakiTraceEventFunc g_trace_cb = NULL;
static void *g_trace_cb_user = NULL;

CHIAKI_EXPORT void chiaki_trace_set_event_cb(ChiakiTraceEventFunc func, void *user)
{
	g_trace_cb_user = user;
	g_trace_cb = func;
}

CHIAKI_EXPORT void chiaki_trace_event(ChiakiTraceEvent event, uint64_t a, uint64_t b)
{
	ChiakiTraceEventFunc cb = g_trace_cb;
	if(cb)
		cb(event, a, b, g_trace_cb_user);
}
