// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "SavedChoices.h"

#include <CoreFoundation/CoreFoundation.h>

#include <cstdlib>

namespace plk::app {

const char *const kChoicesDomain = "fi.madekivi.pleikkari.mac";

namespace {

const char *const kScalerKey = "scaler";
const char *const kSdrColorKey = "sdrColor";
const char *const kHdrOutputKey = "hdrOutput";
const char *const kHdrKey = "hdr";
const char *const kHdrBrightnessKey = "hdrBrightness";
const char *const kHdrToneMapKey = "hdrToneMap";

CFStringRef cfString(const std::string &s)
{
	return CFStringCreateWithCString(kCFAllocatorDefault, s.c_str(), kCFStringEncodingUTF8);
}

bool readString(const std::string &domain, const char *key, std::string *out)
{
	CFStringRef d = cfString(domain), k = cfString(key);
	CFPropertyListRef value = CFPreferencesCopyAppValue(k, d);
	CFRelease(d);
	CFRelease(k);
	if(!value)
		return false;
	bool ok = false;
	if(CFGetTypeID(value) == CFStringGetTypeID())
	{
		char buf[64];
		if(CFStringGetCString((CFStringRef)value, buf, sizeof(buf), kCFStringEncodingUTF8))
		{
			*out = buf;
			ok = true;
		}
	}
	CFRelease(value);
	return ok;
}

// value == nullptr removes the key.
void writeString(const std::string &domain, const char *key, const char *value)
{
	CFStringRef d = cfString(domain), k = cfString(key);
	CFStringRef v = value ? cfString(value) : nullptr;
	CFPreferencesSetAppValue(k, v, d);
	CFPreferencesAppSynchronize(d);
	if(v)
		CFRelease(v);
	CFRelease(d);
	CFRelease(k);
}

} // namespace

SavedChoices loadSavedChoices(const std::string &domain)
{
	SavedChoices c;
	std::string s;
	video::Scaler scaler;
	if(readString(domain, kScalerKey, &s) && video::parseScaler(s, &scaler))
		c.scaler = scaler;
	video::SdrColor color;
	if(readString(domain, kSdrColorKey, &s) && video::parseSdrColor(s, &color))
		c.sdr_color = color;
	video::HdrOutput output;
	if(readString(domain, kHdrOutputKey, &s) && video::parseHdrOutput(s, &output))
		c.hdr_output = output;
	if(readString(domain, kHdrKey, &s) && (s == "on" || s == "off"))
		c.hdr = s == "on";
	if(readString(domain, kHdrBrightnessKey, &s))
	{
		char *end = nullptr;
		const long pct = strtol(s.c_str(), &end, 10);
		if(end != s.c_str() && !*end && pct >= kHdrBrightnessMin && pct <= kHdrBrightnessMax)
			c.hdr_brightness = (int)pct;
	}
	video::HdrToneMap tone_map;
	if(readString(domain, kHdrToneMapKey, &s) && video::parseHdrToneMap(s, &tone_map))
		c.hdr_tone_map = tone_map;
	return c;
}

void saveScaler(video::Scaler scaler, const std::string &domain)
{
	writeString(domain, kScalerKey, video::toString(scaler));
}

void saveSdrColor(video::SdrColor color, const std::string &domain)
{
	writeString(domain, kSdrColorKey, video::toString(color));
}

void saveHdrOutput(video::HdrOutput output, const std::string &domain)
{
	writeString(domain, kHdrOutputKey, video::toString(output));
}

void saveHdr(bool on, const std::string &domain)
{
	writeString(domain, kHdrKey, on ? "on" : "off");
}

void saveHdrBrightness(int percent, const std::string &domain)
{
	writeString(domain, kHdrBrightnessKey, std::to_string(percent).c_str());
}

void saveHdrToneMap(video::HdrToneMap tone_map, const std::string &domain)
{
	writeString(domain, kHdrToneMapKey, video::toString(tone_map));
}

void forgetSavedChoices(const std::string &domain)
{
	for(const char *key : { kScalerKey, kSdrColorKey, kHdrOutputKey, kHdrKey, kHdrBrightnessKey, kHdrToneMapKey })
		writeString(domain, key, nullptr);
}

std::string applySavedChoices(const SavedChoices &saved, Options &o)
{
	std::string taken;
	auto note = [&taken](const std::string &what) { taken += (taken.empty() ? "" : ", ") + what; };
	if(!o.scaler && saved.scaler)
	{
		o.scaler = saved.scaler;
		note(std::string("scaler ") + video::toString(*saved.scaler));
	}
	if(!o.sdr_color && saved.sdr_color)
	{
		o.sdr_color = saved.sdr_color;
		note(std::string("colour ") + video::toString(*saved.sdr_color));
	}
	if(!o.hdr_output && saved.hdr_output)
	{
		o.hdr_output = saved.hdr_output;
		note(std::string("HDR output ") + video::toString(*saved.hdr_output));
	}
	if(!o.hdr_brightness && saved.hdr_brightness)
	{
		o.hdr_brightness = saved.hdr_brightness;
		note("HDR brightness " + std::to_string(*saved.hdr_brightness) + " %");
	}
	if(!o.hdr_tone_map && saved.hdr_tone_map)
	{
		o.hdr_tone_map = saved.hdr_tone_map;
		note(std::string("HDR tone mapping ") + video::toString(*saved.hdr_tone_map));
	}
	// --codec is as explicit about HDR as --hdr is.
	if(!o.hdr && !o.codec && saved.hdr)
	{
		o.hdr = saved.hdr;
		note(*saved.hdr ? "HDR on" : "HDR off");
	}
	return taken;
}

} // namespace plk::app
