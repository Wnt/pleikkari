// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "Registration.h"

#include <CoreFoundation/CoreFoundation.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <pwd.h>
#include <unistd.h>

namespace plk {

namespace {

void wipe(void *p, size_t n)
{
	if(n)
		memset_s(p, n, 0, n);
}

/** Owns one CF reference. */
template <typename T>
struct CFHolder
{
	T ref = nullptr;
	CFHolder() = default;
	explicit CFHolder(T r) : ref(r) {}
	~CFHolder() { if(ref) CFRelease(ref); }
	CFHolder(const CFHolder &) = delete;
	CFHolder &operator=(const CFHolder &) = delete;
};

CFTypeRef lookup(CFDictionaryRef dict, const std::string &key)
{
	CFHolder<CFStringRef> k(CFStringCreateWithCString(kCFAllocatorDefault, key.c_str(), kCFStringEncodingUTF8));
	if(!k.ref)
		return nullptr;
	return CFDictionaryGetValue(dict, k.ref);
}

bool cfStringToUtf8(CFStringRef s, std::string &out)
{
	CFIndex len = CFStringGetLength(s);
	CFIndex max = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
	std::string buf((size_t)max, '\0');
	if(!CFStringGetCString(s, &buf[0], max, kCFStringEncodingUTF8))
		return false;
	buf.resize(strlen(buf.c_str()));
	out.swap(buf);
	return true;
}

/** A QString value: undo QSettings' "@@" escape. */
bool readString(CFTypeRef v, std::string &out)
{
	if(!v)
		return false;
	if(CFGetTypeID(v) == CFStringGetTypeID())
	{
		if(!cfStringToUtf8((CFStringRef)v, out))
			return false;
		if(out.compare(0, 2, "@@") == 0)
			out.erase(0, 1);
		return true;
	}
	if(CFGetTypeID(v) == CFDataGetTypeID())
	{
		// QSettings on macOS stores a QString holding a NUL character as UTF-8 CFData.
		CFDataRef d = (CFDataRef)v;
		out.assign((const char *)CFDataGetBytePtr(d), (size_t)CFDataGetLength(d));
		return true;
	}
	return false;
}

/** A QByteArray value: CFData, or the string form "@ByteArray(...)" (Latin-1). */
bool readBytes(CFTypeRef v, std::vector<uint8_t> &out)
{
	out.clear();
	if(!v)
		return false;
	if(CFGetTypeID(v) == CFDataGetTypeID())
	{
		CFDataRef d = (CFDataRef)v;
		const UInt8 *p = CFDataGetBytePtr(d);
		out.assign(p, p + CFDataGetLength(d));
		return true;
	}
	if(CFGetTypeID(v) == CFStringGetTypeID())
	{
		CFStringRef s = (CFStringRef)v;
		static const char prefix[] = "@ByteArray(";
		const CFIndex prefix_len = (CFIndex)(sizeof(prefix) - 1);
		CFIndex len = CFStringGetLength(s);
		if(len < prefix_len + 1)
			return false;
		std::vector<UniChar> chars((size_t)len);
		CFStringGetCharacters(s, CFRangeMake(0, len), chars.data());
		bool ok = true;
		for(CFIndex i = 0; i < prefix_len; i++)
			ok = ok && chars[(size_t)i] == (UniChar)prefix[i];
		ok = ok && chars[(size_t)len - 1] == ')';
		if(ok)
		{
			for(CFIndex i = prefix_len; i < len - 1; i++)
			{
				if(chars[(size_t)i] > 0xff)
				{
					ok = false;
					break;
				}
				out.push_back((uint8_t)chars[(size_t)i]);
			}
		}
		wipe(chars.data(), chars.size() * sizeof(UniChar));
		if(!ok)
			wipe(out.data(), out.size()), out.clear();
		return ok;
	}
	return false;
}

/** An int/uint value: CFNumber, CFBoolean, or a decimal string (as an .ini import leaves it). */
bool readInt(CFTypeRef v, long long &out)
{
	if(!v)
		return false;
	CFTypeID t = CFGetTypeID(v);
	if(t == CFNumberGetTypeID())
		return CFNumberGetValue((CFNumberRef)v, kCFNumberLongLongType, &out);
	if(t == CFBooleanGetTypeID())
	{
		out = CFBooleanGetValue((CFBooleanRef)v) ? 1 : 0;
		return true;
	}
	if(t == CFStringGetTypeID())
	{
		std::string s;
		if(!cfStringToUtf8((CFStringRef)v, s) || s.empty())
			return false;
		errno = 0;
		char *end = nullptr;
		long long n = strtoll(s.c_str(), &end, 10);
		if(errno || !end || *end)
			return false;
		out = n;
		return true;
	}
	return false;
}

bool readFile(const std::string &path, std::vector<uint8_t> &out, std::string *error)
{
	std::ifstream f(path, std::ios::binary);
	if(!f)
	{
		if(error)
			*error = "cannot open " + path + ": " + strerror(errno);
		return false;
	}
	out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	if(f.bad())
	{
		if(error)
			*error = "cannot read " + path;
		return false;
	}
	return true;
}

} // namespace

RegisteredConsole::~RegisteredConsole()
{
	wipe(regist_key.data(), regist_key.size());
	wipe(morning.data(), morning.size());
}

std::string qtSettingsPlistPath(const std::string &profile)
{
	std::string home;
	const char *env = getenv("HOME");
	if(env && *env)
		home = env;
	else if(const struct passwd *pw = getpwuid(getuid()))
		home = pw->pw_dir ? pw->pw_dir : "";
	std::string domain = "com.chiaki.Chiaki";
	if(!profile.empty())
		domain += "-" + profile;
	return home + "/Library/Preferences/" + domain + ".plist";
}

bool loadRegisteredConsoles(const std::string &plist_path, std::vector<RegisteredConsole> &out, std::string *error)
{
	out.clear();
	std::vector<uint8_t> raw;
	if(!readFile(plist_path, raw, error))
		return false;

	CFHolder<CFDataRef> data(CFDataCreate(kCFAllocatorDefault, raw.data(), (CFIndex)raw.size()));
	wipe(raw.data(), raw.size());
	if(!data.ref)
	{
		if(error)
			*error = "out of memory";
		return false;
	}

	CFErrorRef cf_error = nullptr;
	CFHolder<CFPropertyListRef> plist(CFPropertyListCreateWithData(kCFAllocatorDefault, data.ref,
			kCFPropertyListImmutable, nullptr, &cf_error));
	if(cf_error)
		CFRelease(cf_error); // its description could quote file content, so it is not passed on
	if(!plist.ref || CFGetTypeID(plist.ref) != CFDictionaryGetTypeID())
	{
		if(error)
			*error = plist_path + " is not a property list dictionary";
		return false;
	}
	CFDictionaryRef dict = (CFDictionaryRef)plist.ref;

	long long count = 0;
	if(!readInt(lookup(dict, "registered_hosts.size"), count) || count < 0)
		count = 0; // QSettings::beginReadArray() reads a missing size as 0
	if(count > 4096)
		count = 4096;

	for(long long i = 1; i <= count; i++)
	{
		const std::string prefix = "registered_hosts." + std::to_string(i) + ".";
		RegisteredConsole c;

		readString(lookup(dict, prefix + "server_nickname"), c.nickname);

		long long n = 0;
		if(readInt(lookup(dict, prefix + "target"), n))
			c.target = (ChiakiTarget)n;
		if(readInt(lookup(dict, prefix + "rp_key_type"), n))
			c.rp_key_type = (uint32_t)n;

		std::vector<uint8_t> bytes;
		if(readBytes(lookup(dict, prefix + "server_mac"), bytes) && bytes.size() == c.mac.size())
		{
			memcpy(c.mac.data(), bytes.data(), c.mac.size());
			c.mac_valid = true;
		}
		if(readBytes(lookup(dict, prefix + "rp_regist_key"), bytes) && bytes.size() == c.regist_key.size())
		{
			memcpy(c.regist_key.data(), bytes.data(), c.regist_key.size());
			c.regist_key_valid = true;
		}
		wipe(bytes.data(), bytes.size());
		if(readBytes(lookup(dict, prefix + "rp_key"), bytes) && bytes.size() == c.morning.size())
		{
			memcpy(c.morning.data(), bytes.data(), c.morning.size());
			c.morning_valid = true;
		}
		wipe(bytes.data(), bytes.size());

		out.push_back(c);
	}
	return true;
}

const RegisteredConsole *findConsole(const std::vector<RegisteredConsole> &consoles, const std::string &nickname)
{
	for(const auto &c : consoles)
		if(c.nickname == nickname)
			return &c;
	return nullptr;
}

} // namespace plk
