// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "AwdlGuard.h"

#include <SystemConfiguration/SystemConfiguration.h>
#include <dispatch/dispatch.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <pwd.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern char **environ;

namespace plk {

namespace {

const char *const kAwdl = "awdl0";
const char *const kSudoersFile = "/private/etc/sudoers.d/chiaki-awdl";

// Runs a program and waits for it. Output is dropped: none of these commands says anything
// worth keeping, and sudo -n complains on purpose when the rule is missing.
int run(const std::vector<std::string> &args)
{
	std::vector<char *> argv;
	for(const std::string &a : args)
		argv.push_back(const_cast<char *>(a.c_str()));
	argv.push_back(nullptr);
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
	posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
	pid_t pid = 0;
	const int r = posix_spawn(&pid, argv[0], &fa, nullptr, argv.data(), environ);
	posix_spawn_file_actions_destroy(&fa);
	if(r != 0)
		return -1;
	int status = 0;
	while(waitpid(pid, &status, 0) < 0)
	{
		if(errno != EINTR)
			return -1;
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

bool ifconfigAwdl(const char *what)
{
	return run({"/usr/bin/sudo", "-n", "/sbin/ifconfig", kAwdl, what}) == 0;
}

// The name goes into a sudoers line and a shell command: anything unusual is refused.
bool plainName(const std::string &s)
{
	if(s.empty() || !(isalpha((unsigned char)s[0]) || s[0] == '_'))
		return false;
	for(char c : s)
	{
		if(!(isalnum((unsigned char)c) || c == '_' || c == '-'))
			return false;
	}
	return true;
}

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

// 0 installed (and awdl0 taken down), 1 dialog dismissed, -1 failed
int installRule()
{
	const struct passwd *pw = getpwuid(getuid());
	const std::string user = pw && pw->pw_name ? pw->pw_name : "";
	if(!plainName(user))
		return -1;
	// The rule is checked with visudo before it is installed: a sudoers file with an error in
	// it would switch sudo off altogether. No double quote or backslash below, so the command
	// can sit in an AppleScript string as it is.
	const std::string command =
		"t=$(/usr/bin/mktemp /tmp/chiaki-awdl.XXXXXX)"
		" && /bin/echo '" + user + " ALL=(root) NOPASSWD: /sbin/ifconfig awdl0 down, /sbin/ifconfig awdl0 up' > $t"
		" && /usr/sbin/visudo -cf $t > /dev/null"
		" && /usr/bin/install -m 0440 -o root -g wheel $t " + kSudoersFile +
		" && /bin/rm -f $t"
		" && /sbin/ifconfig awdl0 down";
	const std::string prompt =
		"The stream runs over Wi-Fi, and AWDL (AirDrop, AirPlay, Handoff) takes the Wi-Fi radio "
		"off its channel for up to 100 ms at a time, which makes the picture stutter. "
		"Allow Pleikkari to switch AWDL off while a stream runs? This installs a rule that lets "
		"only the commands 'ifconfig awdl0 down' and 'ifconfig awdl0 up' run without a password.";
	const std::string script = "do shell script \"" + command + "\" with prompt \"" + prompt + "\" with administrator privileges";
	const int r = run({"/usr/bin/osascript", "-e", script});
	if(r == 0)
		return 0;
	// osascript answers 1 both for a dismissed dialog and for a failed command; what tells
	// them apart is whether the rule is there now.
	return access(kSudoersFile, F_OK) == 0 ? -1 : 1;
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
	bool install_tried = false;
	bool gave_up = false;
	bool allow_prompt = true;
	std::atomic<bool> brought_down{false};
	std::atomic<bool> prompt_open{false};

	void tick()
	{
		if(gave_up || users == 0 || AwdlGuard::awdlIsUp() != 1)
			return;
		if(ifconfigAwdl("down"))
		{
			if(!brought_down.exchange(true))
				fprintf(stderr, "[awdl] awdl0 taken down for the stream\n");
			return;
		}
		// sudo -n fails when the rule is not there yet.
		if(install_tried || !allow_prompt)
		{
			gave_up = true;
			return;
		}
		install_tried = true;
		prompt_open = true;
		const int r = installRule();
		prompt_open = false;
		if(r == 0)
		{
			brought_down = true;
			fprintf(stderr, "[awdl] rule installed in %s, awdl0 taken down\n", kSudoersFile);
			// The stream may have ended while the dialog was open.
			if(users == 0)
				bringUp();
			return;
		}
		gave_up = true;
		fprintf(stderr, r == 1 ? "[awdl] password dialog dismissed: AWDL is left alone (--keep-awdl skips the question)\n"
							   : "[awdl] installing the AWDL rule failed: AWDL is left alone\n");
	}

	void bringUp()
	{
		if(!brought_down.exchange(false))
			return;
		ifconfigAwdl("up");
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
	// The queue is busy for as long as the password dialog is open: at exit, do not wait for it.
	if(impl->prompt_open || !impl->brought_down)
		return;
	dispatch_sync(impl->queue, ^{
		impl->users = 0;
		impl->release_generation++;
		impl->setWatching(false);
		impl->bringUp();
	});
}

} // namespace plk
