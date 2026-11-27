// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "AwdlHelperClient.h"

#include "AwdlHelperProtocol.h"

#import <Foundation/Foundation.h>
#import <ServiceManagement/ServiceManagement.h>
#include <xpc/xpc.h>

#include <atomic>
#include <cstring>

namespace plk {

namespace {

xpc_connection_t connection; // used on the caller's queue only
std::atomic<bool> held{false};
bool settings_shown = false;

void drop()
{
	held = false;
	if(connection)
		xpc_connection_cancel(connection);
	connection = nil;
}

// A request and its answer. nil when the helper cannot be reached: it is not registered,
// not allowed, or it is not the helper this app was built with.
xpc_object_t ask(const char *op)
{
	if(!connection)
	{
		xpc_connection_t c = xpc_connection_create_mach_service(PLK_AWDL_HELPER_SERVICE, nullptr, XPC_CONNECTION_MACH_SERVICE_PRIVILEGED);
#ifdef PLK_AWDL_HELPER_REQUIREMENT
		// The name is only a name: talk to nothing but the helper signed with the app.
		if(xpc_connection_set_peer_code_signing_requirement(c, PLK_AWDL_HELPER_REQUIREMENT) != 0)
			return nil;
#endif
		xpc_connection_set_event_handler(c, ^(xpc_object_t event) {
			// The helper went away; its hold went with it (it brings awdl0 back up as it goes).
			if(xpc_get_type(event) == XPC_TYPE_ERROR)
				held = false;
		});
		xpc_connection_resume(c);
		connection = c;
	}
	xpc_object_t message = xpc_dictionary_create(nullptr, nullptr, 0);
	xpc_dictionary_set_string(message, PLK_AWDL_KEY_OP, op);
	xpc_object_t reply = xpc_connection_send_message_with_reply_sync(connection, message);
	if(xpc_get_type(reply) != XPC_TYPE_DICTIONARY)
	{
		drop();
		return nil;
	}
	return reply;
}

std::string describe(NSError *error)
{
	return error ? std::string(error.localizedDescription.UTF8String ?: "") + " (" + error.domain.UTF8String + " " + std::to_string((long)error.code) + ")"
				 : std::string();
}

} // namespace

AwdlHelperClient::Result AwdlHelperClient::hold(bool may_register, std::string &detail)
{
	@autoreleasepool
	{
		detail.clear();
		xpc_object_t reply = ask(PLK_AWDL_OP_HOLD);
		if(reply)
		{
			if(xpc_dictionary_get_bool(reply, PLK_AWDL_KEY_OK))
			{
				held = true;
				return Result::Held;
			}
			const int e = (int)xpc_dictionary_get_int64(reply, PLK_AWDL_KEY_ERRNO);
			detail = std::string("the AWDL helper could not take awdl0 down: ") + strerror(e);
			drop();
			return Result::Unavailable;
		}

		// No answer: the daemon is not registered with macOS, or not allowed yet.
		if(!NSBundle.mainBundle.bundleIdentifier ||
				![NSFileManager.defaultManager fileExistsAtPath:[NSBundle.mainBundle.bundlePath
						stringByAppendingPathComponent:@"Contents/Library/LaunchDaemons/" PLK_AWDL_HELPER_PLIST]])
		{
			detail = "no AWDL helper: this is not Pleikkari.app with its helper inside";
			return Result::Unavailable;
		}
		SMAppService *service = [SMAppService daemonServiceWithPlistName:@PLK_AWDL_HELPER_PLIST];
		if(service.status == SMAppServiceStatusNotRegistered || service.status == SMAppServiceStatusNotFound)
		{
			if(!may_register)
				return Result::Unavailable;
			NSError *error = nil;
			if(![service registerAndReturnError:&error] && service.status != SMAppServiceStatusRequiresApproval)
			{
				detail = "macOS did not register the AWDL helper: " + describe(error) +
						"; it registers the helper of an app signed with a certificate, not of an ad hoc build";
				return Result::Unavailable;
			}
		}
		switch(service.status)
		{
			case SMAppServiceStatusEnabled:
				// Registered a moment ago, or launchd has not started it yet: the next try reaches it.
				reply = ask(PLK_AWDL_OP_HOLD);
				if(reply && xpc_dictionary_get_bool(reply, PLK_AWDL_KEY_OK))
				{
					held = true;
					return Result::Held;
				}
				return Result::NeedsApproval;
			case SMAppServiceStatusRequiresApproval:
				if(may_register && !settings_shown)
				{
					settings_shown = true;
					detail = "the AWDL helper waits to be allowed: System Settings, General, Login Items & Extensions, Pleikkari";
					[SMAppService openSystemSettingsLoginItems];
				}
				return Result::NeedsApproval;
			default:
				detail = "the AWDL helper is not registered";
				return Result::Unavailable;
		}
	}
}

void AwdlHelperClient::release()
{
	@autoreleasepool
	{
		if(!connection)
			return;
		if(held)
			ask(PLK_AWDL_OP_RELEASE);
		drop();
	}
}

std::string AwdlHelperClient::manage(const std::string &action)
{
	@autoreleasepool
	{
		SMAppService *service = [SMAppService daemonServiceWithPlistName:@PLK_AWDL_HELPER_PLIST];
		std::string said;
		NSError *error = nil;
		if(action == "register")
		{
			if(![service registerAndReturnError:&error])
				said = "register: " + describe(error) + "\n";
			if(service.status == SMAppServiceStatusRequiresApproval)
				[SMAppService openSystemSettingsLoginItems];
		}
		else if(action == "unregister")
		{
			if(![service unregisterAndReturnError:&error])
				said = "unregister: " + describe(error) + "\n";
		}
		switch(service.status)
		{
			case SMAppServiceStatusNotRegistered: said += "not registered"; break;
			case SMAppServiceStatusEnabled: said += "enabled"; break;
			case SMAppServiceStatusRequiresApproval:
				said += "waiting to be allowed: System Settings, General, Login Items & Extensions, Pleikkari";
				break;
			case SMAppServiceStatusNotFound: said += "not found: this app has no helper inside, or macOS does not accept it"; break;
		}
		if(service.status == SMAppServiceStatusEnabled)
		{
			xpc_object_t reply = ask(PLK_AWDL_OP_STATUS);
			if(reply)
				said += xpc_dictionary_get_int64(reply, PLK_AWDL_KEY_UP) == 1 ? "; the helper answers, awdl0 is up" : "; the helper answers, awdl0 is down";
			else
				said += "; the helper does not answer";
			drop();
		}
		return said;
	}
}

bool AwdlHelperClient::holding()
{
	return held;
}

} // namespace plk
