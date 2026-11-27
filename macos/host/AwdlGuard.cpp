// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "AwdlGuard.h"

#include "AwdlHelperClient.h"

#include <SystemConfiguration/SystemConfiguration.h>
#include <dispatch/dispatch.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace plk {

namespace {

const char *const kAwdl = "awdl0";

std::string toStd(CFStringRef s)
{
	if(!s || CFGetTypeID(s) != CFStringGetTypeID())
		return std::string();
	char buf[256];
	return CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8) ? std::string(buf) : std::string();
}

// One of the machine's own interfaces (Ethernet, Wi-Fi, a USB adapter), as the system lists them.
bool isHardware(const std::string &ifname)
{
	bool found = false;
	CFArrayRef all = SCNetworkInterfaceCopyAll();
	if(!all)
		return false;
	for(CFIndex i = 0; i < CFArrayGetCount(all) && !found; i++)
		found = toStd(SCNetworkInterfaceGetBSDName((SCNetworkInterfaceRef)CFArrayGetValueAtIndex(all, i))) == ifname;
	CFRelease(all);
	return found;
}

// A VPN's interface: point to point, and not one of the machine's own.
bool isTunnel(const std::string &ifname)
{
	if(ifname.empty() || ifname.size() >= IFNAMSIZ)
		return false;
	const int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if(sock < 0)
		return false;
	struct ifreq ifr = {};
	strlcpy(ifr.ifr_name, ifname.c_str(), sizeof(ifr.ifr_name));
	const int r = ioctl(sock, SIOCGIFFLAGS, &ifr);
	close(sock);
	return r == 0 && (ifr.ifr_flags & IFF_POINTOPOINT) && !(ifr.ifr_flags & IFF_LOOPBACK);
}

// The services that are connected through a router, with their interfaces, and the system's
// order of preference among services, from the dynamic store (what scutil shows).
void activeServices(std::vector<std::string> &order, std::vector<std::pair<std::string, std::string>> &active)
{
	SCDynamicStoreRef store = SCDynamicStoreCreate(kCFAllocatorDefault, CFSTR("fi.madekivi.pleikkari.awdl"), nullptr, nullptr);
	if(!store)
		return;
	CFStringRef setup_key = SCDynamicStoreKeyCreateNetworkGlobalEntity(kCFAllocatorDefault, kSCDynamicStoreDomainSetup, kSCEntNetIPv4);
	CFPropertyListRef setup = SCDynamicStoreCopyValue(store, setup_key);
	if(setup && CFGetTypeID(setup) == CFDictionaryGetTypeID())
	{
		CFArrayRef ids = (CFArrayRef)CFDictionaryGetValue((CFDictionaryRef)setup, kSCPropNetServiceOrder);
		for(CFIndex i = 0; ids && CFGetTypeID(ids) == CFArrayGetTypeID() && i < CFArrayGetCount(ids); i++)
			order.push_back(toStd((CFStringRef)CFArrayGetValueAtIndex(ids, i)));
	}
	if(setup)
		CFRelease(setup);
	CFRelease(setup_key);

	const CFStringRef entities[][2] = {{kSCEntNetIPv4, kSCPropNetIPv4Router}, {kSCEntNetIPv6, kSCPropNetIPv6Router}};
	for(const auto &protocol : entities)
	{
		const CFStringRef entity = protocol[0], router = protocol[1];
		CFStringRef pattern = SCDynamicStoreKeyCreateNetworkServiceEntity(kCFAllocatorDefault, kSCDynamicStoreDomainState, kSCCompAnyRegex, entity);
		CFArrayRef patterns = CFArrayCreate(kCFAllocatorDefault, (const void **)&pattern, 1, &kCFTypeArrayCallBacks);
		CFDictionaryRef found = SCDynamicStoreCopyMultiple(store, nullptr, patterns);
		const CFIndex n = found ? CFDictionaryGetCount(found) : 0;
		std::vector<const void *> keys(n), values(n);
		if(n > 0)
			CFDictionaryGetKeysAndValues(found, keys.data(), values.data());
		for(CFIndex i = 0; i < n; i++)
		{
			if(CFGetTypeID(values[i]) != CFDictionaryGetTypeID())
				continue;
			CFDictionaryRef service = (CFDictionaryRef)values[i];
			// Without a router the service leads nowhere (a cable with a self-assigned address).
			if(!CFDictionaryContainsKey(service, router))
				continue;
			const std::string ifname = toStd((CFStringRef)CFDictionaryGetValue(service, kSCPropInterfaceName));
			// "State:/Network/Service/<identifier>/IPv4"
			const std::string key = toStd((CFStringRef)keys[i]);
			const size_t end = key.rfind('/');
			const size_t begin = end == std::string::npos || end == 0 ? std::string::npos : key.rfind('/', end - 1);
			if(ifname.empty() || begin == std::string::npos)
				continue;
			const std::pair<std::string, std::string> entry(key.substr(begin + 1, end - begin - 1), ifname);
			if(std::find(active.begin(), active.end(), entry) == active.end())
				active.push_back(entry);
		}
		if(found)
			CFRelease(found);
		CFRelease(patterns);
		CFRelease(pattern);
	}
	CFRelease(store);
}

} // namespace

std::string AwdlGuard::outgoingInterface(const std::string &host)
{
	struct addrinfo hints = {};
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = AI_NUMERICHOST;
	struct addrinfo *res = nullptr;
	// A name is not resolved here (that could block): the default route answers instead.
	if(getaddrinfo(host.c_str(), "9296", &hints, &res) != 0 || !res)
	{
		res = nullptr;
		if(getaddrinfo("1.1.1.1", "9296", &hints, &res) != 0 || !res)
			return std::string();
	}
	std::string name;
	// Connecting a UDP socket sends nothing: it only asks the routing table.
	const int sock = socket(res->ai_family, SOCK_DGRAM, 0);
	if(sock >= 0)
	{
		struct sockaddr_storage local = {};
		socklen_t local_len = sizeof(local);
		if(connect(sock, res->ai_addr, res->ai_addrlen) == 0 && getsockname(sock, (struct sockaddr *)&local, &local_len) == 0)
		{
			struct ifaddrs *ifs = nullptr;
			if(getifaddrs(&ifs) == 0)
			{
				for(struct ifaddrs *i = ifs; i && name.empty(); i = i->ifa_next)
				{
					if(!i->ifa_addr || i->ifa_addr->sa_family != local.ss_family)
						continue;
					bool same = false;
					if(local.ss_family == AF_INET)
						same = ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr == ((struct sockaddr_in *)&local)->sin_addr.s_addr;
					else if(local.ss_family == AF_INET6)
						same = memcmp(&((struct sockaddr_in6 *)i->ifa_addr)->sin6_addr, &((struct sockaddr_in6 *)&local)->sin6_addr, sizeof(struct in6_addr)) == 0;
					if(same)
						name = i->ifa_name;
				}
				freeifaddrs(ifs);
			}
		}
		close(sock);
	}
	freeaddrinfo(res);
	return name;
}

std::string AwdlGuard::firstHardwareInterface(const std::vector<std::string> &order,
		const std::vector<std::pair<std::string, std::string>> &active,
		const std::function<bool(const std::string &)> &hardware)
{
	for(const std::string &id : order)
	{
		for(const auto &service : active)
		{
			if(service.first == id && hardware(service.second))
				return service.second;
		}
	}
	// A service the order does not name comes after those it does.
	for(const auto &service : active)
	{
		if(std::find(order.begin(), order.end(), service.first) == order.end() && hardware(service.second))
			return service.second;
	}
	return std::string();
}

std::string AwdlGuard::physicalInterface(const std::string &ifname)
{
	if(ifname.empty())
		return std::string();
	if(isHardware(ifname))
		return ifname;
	if(!isTunnel(ifname))
		return std::string();
	std::vector<std::string> order;
	std::vector<std::pair<std::string, std::string>> active;
	activeServices(order, active);
	return firstHardwareInterface(order, active, isHardware);
}

bool AwdlGuard::isWifi(const std::string &ifname)
{
	bool wifi = false;
	CFArrayRef all = SCNetworkInterfaceCopyAll();
	if(!all)
		return false;
	CFStringRef wanted = CFStringCreateWithCString(kCFAllocatorDefault, ifname.c_str(), kCFStringEncodingUTF8);
	for(CFIndex i = 0; wanted && i < CFArrayGetCount(all) && !wifi; i++)
	{
		SCNetworkInterfaceRef iface = (SCNetworkInterfaceRef)CFArrayGetValueAtIndex(all, i);
		CFStringRef bsd = SCNetworkInterfaceGetBSDName(iface);
		CFStringRef type = SCNetworkInterfaceGetInterfaceType(iface);
		if(bsd && type && CFEqual(bsd, wanted) && CFEqual(type, kSCNetworkInterfaceTypeIEEE80211))
			wifi = true;
	}
	if(wanted)
		CFRelease(wanted);
	CFRelease(all);
	return wifi;
}

int AwdlGuard::awdlIsUp()
{
	const int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if(sock < 0)
		return -1;
	struct ifreq ifr = {};
	strlcpy(ifr.ifr_name, kAwdl, sizeof(ifr.ifr_name));
	const int r = ioctl(sock, SIOCGIFFLAGS, &ifr);
	close(sock);
	if(r < 0)
		return -1;
	return (ifr.ifr_flags & IFF_UP) ? 1 : 0;
}

// Everything below runs on queue, except the atomics.
struct AwdlGuard::Impl
{
	dispatch_queue_t queue = dispatch_queue_create("fi.madekivi.pleikkari.awdl", DISPATCH_QUEUE_SERIAL);
	dispatch_source_t watch = nullptr;
	int users = 0;
	uint64_t release_generation = 0;
	bool watching = false;
	bool gave_up = false;
	bool allow_prompt = true;
	bool waiting_said = false;
	std::atomic<bool> brought_down{false};

	// Once a second while a stream runs, until the helper holds awdl0 down. After that the
	// helper keeps it down by itself; this only notices when the helper is gone.
	void tick()
	{
		if(gave_up || users == 0 || AwdlHelperClient::holding())
			return;
		std::string detail;
		switch(AwdlHelperClient::hold(allow_prompt, detail))
		{
			case AwdlHelperClient::Result::Held:
				if(!brought_down.exchange(true))
					fprintf(stderr, "[awdl] awdl0 taken down for the stream\n");
				waiting_said = false;
				break;
			case AwdlHelperClient::Result::NeedsApproval:
				// The user may allow it while the stream runs: keep asking.
				if(!detail.empty())
					fprintf(stderr, "[awdl] %s\n", detail.c_str());
				if(!waiting_said)
					fprintf(stderr, "[awdl] AWDL stays up until the helper is allowed\n");
				waiting_said = true;
				brought_down = false;
				break;
			case AwdlHelperClient::Result::Unavailable:
				gave_up = true;
				brought_down = false;
				fprintf(stderr, "[awdl] %s: AWDL is left alone\n", detail.empty() ? "no AWDL helper" : detail.c_str());
				break;
		}
	}

	void bringUp()
	{
		const bool was_down = brought_down.exchange(false);
		AwdlHelperClient::release();
		if(was_down)
			fprintf(stderr, "[awdl] stream ended: awdl0 brought back up\n");
	}

	void setWatching(bool on)
	{
		if(on == watching)
			return;
		watching = on;
		if(on)
			dispatch_resume(watch);
		else
			dispatch_suspend(watch);
	}
};

AwdlGuard::AwdlGuard() : impl_(new Impl)
{
	Impl *impl = impl_;
	impl->watch = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, impl->queue);
	dispatch_source_set_timer(impl->watch, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, NSEC_PER_SEC / 10);
	dispatch_source_set_event_handler(impl->watch, ^{
		impl->tick();
	});
	// A timer source is created suspended; setWatching() resumes it.
	atexit([] { AwdlGuard::instance().restoreNow(); });
}

AwdlGuard &AwdlGuard::instance()
{
	static AwdlGuard *guard = new AwdlGuard; // never destroyed: its queue outlives every user
	return *guard;
}

bool AwdlGuard::acquire(const std::string &host, bool allow_prompt)
{
	// A stream through a VPN leaves from the tunnel's interface, and the tunnel's packets from
	// a hardware one: the radio that AWDL takes away is under both.
	const std::string ifname = outgoingInterface(host);
	const std::string physical = physicalInterface(ifname);
	if(physical.empty() || !isWifi(physical) || awdlIsUp() < 0)
		return false;
	const std::string path = physical == ifname ? "over Wi-Fi" : "through " + ifname + " over Wi-Fi (" + physical + ")";
	Impl *impl = impl_;
	dispatch_async(impl->queue, ^{
		impl->release_generation++; // a pending bring-up is called off
		impl->allow_prompt = allow_prompt;
		if(impl->users++ == 0 && !impl->watching)
			fprintf(stderr, "[awdl] stream runs %s: keeping awdl0 down while it lasts\n", path.c_str());
		impl->tick();
		if(!impl->gave_up)
			impl->setWatching(true);
	});
	return true;
}

void AwdlGuard::release()
{
	Impl *impl = impl_;
	dispatch_async(impl->queue, ^{
		if(impl->users == 0 || --impl->users > 0)
			return;
		// The interface comes back a moment after the last stream ended, so that a reconnect
		// does not switch it up and down.
		const uint64_t generation = ++impl->release_generation;
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), impl->queue, ^{
			if(impl->release_generation != generation || impl->users > 0)
				return;
			impl->setWatching(false);
			impl->bringUp();
		});
	});
}

void AwdlGuard::restoreNow()
{
	Impl *impl = impl_;
	if(!impl->brought_down)
		return;
	dispatch_sync(impl->queue, ^{
		impl->users = 0;
		impl->release_generation++;
		impl->setWatching(false);
		impl->bringUp();
	});
}

} // namespace plk
