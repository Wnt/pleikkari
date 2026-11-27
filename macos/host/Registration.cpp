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

/** A float/double value: CFNumber, or a decimal string (QSettings writes some floats as QString). */
bool readDouble(CFTypeRef v, double &out)
{
	if(!v)
		return false;
	CFTypeID t = CFGetTypeID(v);
	if(t == CFNumberGetTypeID())
		return CFNumberGetValue((CFNumberRef)v, kCFNumberDoubleType, &out);
	if(t == CFStringGetTypeID())
	{
		std::string s;
		if(!cfStringToUtf8((CFStringRef)v, s) || s.empty())
			return false;
		errno = 0;
		char *end = nullptr;
		double d = strtod(s.c_str(), &end);
		if(errno || !end || *end)
			return false;
		out = d;
		return true;
	}
	return false;
}

/** A bool value: CFBoolean, CFNumber, or the strings QVariant::toBool() accepts. */
bool readBool(CFTypeRef v, bool &out)
{
	if(!v)
		return false;
	CFTypeID t = CFGetTypeID(v);
	if(t == CFBooleanGetTypeID())
	{
		out = CFBooleanGetValue((CFBooleanRef)v);
		return true;
	}
	if(t == CFNumberGetTypeID())
	{
		long long n = 0;
		if(!CFNumberGetValue((CFNumberRef)v, kCFNumberLongLongType, &n))
			return false;
		out = n != 0;
		return true;
	}
	if(t == CFStringGetTypeID())
	{
		std::string s;
		if(!cfStringToUtf8((CFStringRef)v, s))
			return false;
		if(s == "true" || s == "1")
			out = true;
		else if(s == "false" || s == "0" || s.empty())
			out = false;
		else
			return false;
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

namespace {

/** The plist as a dictionary (caller releases), or nullptr with *error set (never quoting the file). */
CFDictionaryRef loadPlistDictionary(const std::string &plist_path, std::string *error)
{
	std::vector<uint8_t> raw;
	if(!readFile(plist_path, raw, error))
		return nullptr;

	CFHolder<CFDataRef> data(CFDataCreate(kCFAllocatorDefault, raw.data(), (CFIndex)raw.size()));
	wipe(raw.data(), raw.size());
	if(!data.ref)
	{
		if(error)
			*error = "out of memory";
		return nullptr;
	}

	CFErrorRef cf_error = nullptr;
	CFPropertyListRef plist = CFPropertyListCreateWithData(kCFAllocatorDefault, data.ref,
			kCFPropertyListImmutable, nullptr, &cf_error);
	if(cf_error)
		CFRelease(cf_error); // its description could quote file content, so it is not passed on
	if(!plist || CFGetTypeID(plist) != CFDictionaryGetTypeID())
	{
		if(plist)
			CFRelease(plist);
		if(error)
			*error = plist_path + " is not a property list dictionary";
		return nullptr;
	}
	return (CFDictionaryRef)plist;
}

} // namespace

bool loadRegisteredConsoles(const std::string &plist_path, std::vector<RegisteredConsole> &out, std::string *error)
{
	out.clear();
	CFHolder<CFDictionaryRef> plist(loadPlistDictionary(plist_path, error));
	if(!plist.ref)
		return false;
	CFDictionaryRef dict = plist.ref;

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

bool loadManualHosts(const std::string &plist_path, std::vector<std::string> &out, std::string *error)
{
	out.clear();
	CFHolder<CFDictionaryRef> plist(loadPlistDictionary(plist_path, error));
	if(!plist.ref)
		return false;
	long long count = 0;
	if(!readInt(lookup(plist.ref, "manual_hosts.size"), count) || count < 0)
		count = 0;
	if(count > 4096)
		count = 4096;
	for(long long i = 1; i <= count; i++)
	{
		std::string host;
		if(readString(lookup(plist.ref, "manual_hosts." + std::to_string(i) + ".host"), host) && !host.empty())
			out.push_back(host);
	}
	return true;
}

bool loadQtStreamSettings(const std::string &plist_path, QtStreamSettings &out, std::string *error)
{
	out = QtStreamSettings();
	CFHolder<CFDictionaryRef> plist(loadPlistDictionary(plist_path, error));
	if(!plist.ref)
		return false;
	CFDictionaryRef dict = plist.ref;

	std::string str;
	if(readString(lookup(dict, "settings.resolution_local_ps5"), str)
		&& (str == "360p" || str == "540p" || str == "720p" || str == "1080p"))
	{
		out.resolution = str;
		out.has_resolution = true;
	}
	long long n = 0;
	if(readInt(lookup(dict, "settings.fps_local_ps5"), n) && (n == 30 || n == 60))
	{
		out.fps = (int)n;
		out.has_fps = true;
	}
	if(readInt(lookup(dict, "settings.bitrate_local_ps5"), n) && n >= 0 && n <= 1000000)
	{
		out.bitrate_kbps = (unsigned)n;
		out.has_bitrate = true;
	}
	if(readString(lookup(dict, "settings.codec_local_ps5"), str) && (str == "h264" || str == "h265" || str == "h265_hdr"))
	{
		out.codec = str;
		out.has_codec = true;
	}
	double d = 0;
	if(readDouble(lookup(dict, "settings.haptic_override"), d) && d >= 0.0 && d <= 10.0)
	{
		out.haptic_gain = (float)d;
		out.has_haptic_gain = true;
	}
	bool b = false;
	if(readBool(lookup(dict, "settings.dualsense_bt_haptics"), b))
	{
		out.bt_haptics = b;
		out.has_bt_haptics = true;
	}
	// Settings::GetPacketLossReportedMax(): the newer key wins over the older one.
	if((readDouble(lookup(dict, "settings.packet_loss_reported_max"), d) || readDouble(lookup(dict, "settings.packet_loss_max"), d))
		&& d >= 0.0 && d <= 1.0)
	{
		out.packet_loss_max = d;
		out.has_packet_loss_max = true;
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
