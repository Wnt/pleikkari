// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#pragma once

#include <pthread.h>
#include <pthread/qos.h>

namespace plk::dualsense {

// Names the calling thread and puts it in QOS_CLASS_USER_INTERACTIVE: the highest
// QoS a normal process can request. It keeps the thread on the performance cores and
// gives its timers the smallest coalescing leeway, which matters for the paced
// haptics sender and for a wake-up on every input report.
inline void SetCurrentThreadRealtimeish(const char *name)
{
	pthread_setname_np(name);
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
}

} // namespace plk::dualsense
