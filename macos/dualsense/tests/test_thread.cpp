// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// The kernel accepts the time-constraint parameters the driver threads use
// (Manager::Options::realtime_threads), and the policy takes effect.

#include "HapticsSender.h"
#include "ThreadUtil.h"
#include "test.h"

#include <mach/mach.h>
#include <mach/thread_act.h>

#include <thread>

using namespace plk::dualsense;

namespace {

int CurrentPriority()
{
	thread_extended_info_data_t info;
	mach_msg_type_number_t count = THREAD_EXTENDED_INFO_COUNT;
	mach_port_t self = mach_thread_self();
	const kern_return_t kr = thread_info(self, THREAD_EXTENDED_INFO, (thread_info_t)&info, &count);
	mach_port_deallocate(mach_task_self(), self);
	return kr == KERN_SUCCESS ? info.pth_curpri : -1;
}

bool AcceptedOnFreshThread(uint64_t period, uint64_t computation, uint64_t constraint, int *priority)
{
	bool ok = false;
	std::thread t([&] {
		ok = SetCurrentThreadTimeConstraint(period, computation, constraint);
		*priority = CurrentPriority();
	});
	t.join();
	return ok;
}

} // namespace

TEST(thread_time_constraint_parameters_are_accepted)
{
	int pri = 0;
	CHECK(AcceptedOnFreshThread(0, kHidThreadComputationNs, kHidThreadConstraintNs, &pri));
	CHECK(pri >= 90); // real-time band (97 observed), above every QoS class (<= 47)
	CHECK(AcceptedOnFreshThread(0, kWriterThreadComputationNs, kWriterThreadConstraintNs, &pri));
	for(bool double_block : { true, false })
	{
		HapticsConfig c;
		c.double_block = double_block;
		HapticsPacer pacer(c);
		CHECK(AcceptedOnFreshThread((uint64_t)pacer.period(), kHapticsThreadComputationNs, kHapticsThreadConstraintNs,
			&pri));
		CHECK(pri >= 90);
	}
	// Nonsense (computation above the constraint) is refused, and the setup helper
	// then leaves the thread at its QoS instead of failing.
	CHECK(!AcceptedOnFreshThread(0, 3000000, 1000000, &pri));
}
