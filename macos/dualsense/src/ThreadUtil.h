// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#pragma once

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <pthread/qos.h>

#include <cstdint>

namespace plk::dualsense {

// Names the calling thread and puts it in QOS_CLASS_USER_INTERACTIVE: the highest
// QoS a normal process can request. It keeps the thread on the performance cores and
// gives its timers the smallest coalescing leeway, which matters for the paced
// haptics sender and for a wake-up on every input report.
//
// The request can be clamped by the process context: run from a background shell,
// the probe's HID thread reported QoS user-interactive but kernel priority 31 (the
// default level, base 47 expected) and 2 ms timers woke ~0.4 ms late at the median.
// SetCurrentThreadTimeConstraint() below is not subject to that clamp.
inline void SetCurrentThreadRealtimeish(const char *name)
{
	pthread_setname_np(name);
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
}

inline uint64_t NanosToMach(uint64_t ns)
{
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	return ns * tb.denom / tb.numer;
}

// Mach time-constraint (real-time) policy for the calling thread, as Core Audio uses
// for its I/O threads: the thread is scheduled ahead of every QoS class and its
// timers get no coalescing leeway. `period_ns` is the expected wake-up interval
// (0 = aperiodic), `computation_ns` the CPU time needed per wake-up and
// `constraint_ns` the deadline for that work. The kernel demotes a thread that keeps
// overrunning its computation budget, so a misbehaving callback cannot starve the
// system. Returns false if the kernel refused the parameters.
inline bool SetCurrentThreadTimeConstraint(uint64_t period_ns, uint64_t computation_ns, uint64_t constraint_ns)
{
	thread_time_constraint_policy_data_t policy;
	policy.period = (uint32_t)NanosToMach(period_ns);
	policy.computation = (uint32_t)NanosToMach(computation_ns);
	policy.constraint = (uint32_t)NanosToMach(constraint_ns);
	policy.preemptible = 1;
	mach_port_t self = pthread_mach_thread_np(pthread_self());
	return thread_policy_set(self, THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&policy,
		THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS;
}

// Thread setup used by the HID, writer and haptics threads: name + user-interactive
// QoS, and with `time_constraint` also the real-time policy (falls back to the QoS
// alone if the kernel refuses it).
inline void SetupDriverThread(const char *name, bool time_constraint, uint64_t period_ns, uint64_t computation_ns,
	uint64_t constraint_ns)
{
	SetCurrentThreadRealtimeish(name);
	if(time_constraint)
		SetCurrentThreadTimeConstraint(period_ns, computation_ns, constraint_ns);
}

// Parameters per thread. Computation is CPU time per wake-up, not wall time: a
// blocking IOHIDDeviceSetReport sleeps and does not count against it.
constexpr uint64_t kHidThreadComputationNs = 500000;     // parse + client callback
constexpr uint64_t kHidThreadConstraintNs = 2000000;
constexpr uint64_t kWriterThreadComputationNs = 500000;  // pack + start the write
constexpr uint64_t kWriterThreadConstraintNs = 2000000;
constexpr uint64_t kHapticsThreadComputationNs = 1000000;
constexpr uint64_t kHapticsThreadConstraintNs = 5000000;

} // namespace plk::dualsense
