// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

/*
 * Pleikkari's AWDL helper: a launch daemon that keeps awdl0 down while a client holds it.
 * See AwdlHelperProtocol.h for what it is asked and why it exists.
 *
 * It takes no arguments, reads no file and runs no program. The only thing it changes on the
 * machine is the IFF_UP flag of the interface named awdl0.
 */

#include "AwdlHelperProtocol.h"

#include <dispatch/dispatch.h>
#include <xpc/xpc.h>

#include <errno.h>
#include <net/if.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <syslog.h>
#include <unistd.h>

static const char *const kAwdl = "awdl0";

// Everything below runs on the main queue.
static int holds = 0;               // connections that hold awdl0 down
static int connections = 0;
static bool taken_down = false;     // this process took the interface down
static dispatch_source_t watch = NULL;
static uint64_t idle_generation = 0;

/** 1 up, 0 down, -1 no such interface (errno set) */
static int awdl_is_up(void)
{
	const int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if(sock < 0)
		return -1;
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, kAwdl, sizeof(ifr.ifr_name));
	const int r = ioctl(sock, SIOCGIFFLAGS, &ifr);
	const int e = errno;
	close(sock);
	errno = e;
	if(r < 0)
		return -1;
	return (ifr.ifr_flags & IFF_UP) ? 1 : 0;
}

/** What "ifconfig awdl0 up|down" does. 0, or an errno. */
static int awdl_set_up(bool up)
{
	const int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if(sock < 0)
		return errno;
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, kAwdl, sizeof(ifr.ifr_name));
	int e = 0;
	if(ioctl(sock, SIOCGIFFLAGS, &ifr) < 0)
		e = errno;
	else if(((ifr.ifr_flags & IFF_UP) != 0) != up)
	{
		if(up)
			ifr.ifr_flags |= IFF_UP;
		else
			ifr.ifr_flags &= ~IFF_UP;
		if(ioctl(sock, SIOCSIFFLAGS, &ifr) < 0)
			e = errno;
	}
	close(sock);
	return e;
}

static int take_down(void)
{
	if(awdl_is_up() != 1)
		return 0;
	const int e = awdl_set_up(false);
	if(e == 0)
	{
		if(!taken_down)
			syslog(LOG_NOTICE, "awdl0 taken down for %d client(s)", holds);
		taken_down = true;
	}
	return e;
}

static void restore(void)
{
	if(!taken_down)
		return;
	taken_down = false;
	const int e = awdl_set_up(true);
	if(e)
		syslog(LOG_ERR, "bringing awdl0 back up failed: %s", strerror(e));
	else
		syslog(LOG_NOTICE, "awdl0 brought back up");
}

static void set_watching(bool on)
{
	if(on == (watch != NULL))
		return;
	if(!on)
	{
		dispatch_source_cancel(watch);
		dispatch_release(watch);
		watch = NULL;
		return;
	}
	// macOS raises the interface again by itself (AirDrop, a Handoff, a wake): look once a second.
	watch = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
	dispatch_source_set_timer(watch, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, NSEC_PER_SEC / 10);
	dispatch_source_set_event_handler(watch, ^{
		if(holds > 0)
			take_down();
	});
	dispatch_resume(watch);
}

static void hold_changed(void)
{
	if(holds > 0)
	{
		set_watching(true);
		return;
	}
	set_watching(false);
	restore();
}

// launchd starts the daemon again when a client connects: with nobody connected there is
// nothing to stay for.
static void exit_when_idle(void)
{
	const uint64_t generation = ++idle_generation;
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		if(generation != idle_generation || connections > 0)
			return;
		restore();
		exit(0);
	});
}

static void accept_client(xpc_connection_t peer)
{
#ifdef PLK_AWDL_CLIENT_REQUIREMENT
	// Only Pleikkari, signed by the team this helper was built for, is listened to. A message
	// from anything else never reaches the handler below.
	if(xpc_connection_set_peer_code_signing_requirement(peer, PLK_AWDL_CLIENT_REQUIREMENT) != 0)
	{
		syslog(LOG_ERR, "the client requirement could not be set: connection refused");
		xpc_connection_cancel(peer);
		return;
	}
#endif
	connections++;
	idle_generation++;
	__block bool holding = false;
	xpc_connection_set_target_queue(peer, dispatch_get_main_queue());
	xpc_connection_set_event_handler(peer, ^(xpc_object_t event) {
		const xpc_type_t type = xpc_get_type(event);
		if(type == XPC_TYPE_ERROR)
		{
			// The client went away, in order or not: its hold goes with it.
			if(event != XPC_ERROR_CONNECTION_INVALID)
				return;
			if(holding)
			{
				holding = false;
				holds--;
				hold_changed();
			}
			if(--connections == 0)
				exit_when_idle();
			return;
		}
		if(type != XPC_TYPE_DICTIONARY)
			return;
		xpc_object_t reply = xpc_dictionary_create_reply(event);
		if(!reply)
			return;
		const char *op = xpc_dictionary_get_string(event, PLK_AWDL_KEY_OP);
		int e = 0;
		if(op && !strcmp(op, PLK_AWDL_OP_HOLD))
		{
			if(!holding)
			{
				holding = true;
				holds++;
			}
			e = take_down();
			if(e == 0 && awdl_is_up() < 0)
				e = ENXIO;
			hold_changed();
		}
		else if(op && !strcmp(op, PLK_AWDL_OP_RELEASE))
		{
			if(holding)
			{
				holding = false;
				holds--;
				hold_changed();
			}
		}
		else if(!op || strcmp(op, PLK_AWDL_OP_STATUS))
			e = EINVAL;
		xpc_dictionary_set_bool(reply, PLK_AWDL_KEY_OK, e == 0);
		xpc_dictionary_set_int64(reply, PLK_AWDL_KEY_UP, awdl_is_up());
		if(e)
			xpc_dictionary_set_int64(reply, PLK_AWDL_KEY_ERRNO, e);
		xpc_connection_send_message(peer, reply);
		xpc_release(reply);
	});
	xpc_connection_resume(peer);
}

int main(void)
{
	openlog("pleikkari-awdl-helper", LOG_PID, LOG_DAEMON);
	// Told to stop (the user switched it off in System Settings, a shutdown): AWDL comes back.
	signal(SIGTERM, SIG_IGN);
	dispatch_source_t term = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_main_queue());
	dispatch_source_set_event_handler(term, ^{
		restore();
		exit(0);
	});
	dispatch_resume(term);

	xpc_connection_t listener = xpc_connection_create_mach_service(PLK_AWDL_HELPER_SERVICE, dispatch_get_main_queue(),
			XPC_CONNECTION_MACH_SERVICE_LISTENER);
	xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
		if(xpc_get_type(peer) == XPC_TYPE_CONNECTION)
			accept_client((xpc_connection_t)peer);
	});
	xpc_connection_resume(listener);
	exit_when_idle();
	dispatch_main();
}
