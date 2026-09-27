// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_JNI_LATENCY_TRACE_H
#define CHIAKI_JNI_LATENCY_TRACE_H

// PLE-746: atrace markers along the Oculus Go's input and video paths (Android 7.1 has atrace but
// no Perfetto). Armed only while the input-to-photon probe runs (stream_go_vr_latency_probe), and
// then only written while atrace records this app (`atrace -a fi.madekivi.pleikkari ...`), so a
// normal stream never formats a marker. Every marker starts "PLE746 " so a trace greps cleanly.

#include <stdbool.h>

void android_chiaki_latency_trace_arm(bool armed);
/** The probe runs; one relaxed load, for the hot paths to return early. */
bool android_chiaki_latency_trace_armed(void);
/** Armed and atrace is recording this app. */
bool android_chiaki_latency_trace_on(void);
/** A zero-length section, the instant marker atrace can show on Android 7.1 (no ATrace_setCounter). */
void android_chiaki_latency_trace_mark(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/** A section on this thread; pair every begin that returned true with an end. */
bool android_chiaki_latency_trace_begin(const char *name);
void android_chiaki_latency_trace_end(void);

#endif
