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
const char *const kResolutionKey = "streamResolution";
const char *const kBitrateKey = "streamBitrate";
const char *const kHdrBrightnessKey = "hdrBrightness";
const char *const kHdrToneMapKey = "hdrToneMap";
const char *const kFullscreenSyncKey = "fullscreenSync";
const char *const kNrKey = "nr";
const char *const kNrNetworkKey = "nrNetwork";
const char *const kNrHeightKey = "nrHeight";
const char *const kNrStrengthKey = "nrStrength";
const char *const kNrLookKey = "nrLook";
const char *const kFpsOverlayKey = "fpsOverlay";
const char *const kNrTimingKey = "nrTiming";
const char *const kFrameGenerationKey = "frameGeneration";
const char *const kCameraWarpKey = "cameraWarp";
const char *const kFrameGenerationFactorKey = "frameGenerationFactor";
const char *const kPanelPictureKey = "panelPicture";

// A whole number within a range; false for anything else.
bool parseInt(const std::string &s, long min, long max, int *out)
{
	char *end = nullptr;
	const long value = strtol(s.c_str(), &end, 10);
	if(end == s.c_str() || *end || value < min || value > max)
		return false;
	*out = (int)value;
	return true;
}

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
	ChiakiVideoResolutionPreset resolution;
	if(readString(domain, kResolutionKey, &s) && parseResolution(s, &resolution))
		c.resolution = resolution;
	if(readString(domain, kBitrateKey, &s))
	{
		char *end = nullptr;
		const unsigned long kbps = strtoul(s.c_str(), &end, 10);
		if(end && !*end && kbps >= 1000 && kbps <= 100000)
			c.bitrate_kbps = (unsigned)kbps;
	}
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
	if(readString(domain, kFullscreenSyncKey, &s) && (s == "on" || s == "off"))
		c.fullscreen_sync = s == "on";
	if(readString(domain, kNrKey, &s) && (s == "on" || s == "off"))
		c.nr = s == "on";
	NrNetwork network;
	if(readString(domain, kNrNetworkKey, &s) && parseNrNetwork(s, &network))
		c.nr_network = network;
	int number = 0;
	if(readString(domain, kNrHeightKey, &s) && parseInt(s, 0, kNrHeightMax, &number) && (number == 0 || number >= kNrHeightMin))
		c.nr_height = number;
	if(readString(domain, kNrStrengthKey, &s) && parseInt(s, kNrStrengthMin, kNrStrengthMax, &number))
		c.nr_strength = number;
	NrLook look;
	if(readString(domain, kNrLookKey, &s) && parseNrLook(s, &look))
		c.nr_look = look;
	if(readString(domain, kCameraWarpKey, &s) && (s == "on" || s == "off"))
		c.camera_warp = s == "on";
	if(readString(domain, kFrameGenerationKey, &s) && (s == "on" || s == "off"))
		c.frame_generation = s == "on";
	if(readString(domain, kFrameGenerationFactorKey, &s) && (s == "2" || s == "4"))
		c.frame_generation_factor = s == "4" ? 4 : 2;
	if(readString(domain, kFpsOverlayKey, &s) && (s == "on" || s == "off"))
		c.fps_overlay = s == "on";
	NrTiming timing;
	if(readString(domain, kNrTimingKey, &s) && parseNrTiming(s, &timing))
		c.nr_timing = timing;
	if(readString(domain, kPanelPictureKey, &s) && (s == "beside" || s == "under"))
		c.panel_beside = s == "beside";
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

void saveFullscreenSync(bool on, const std::string &domain)
{
	writeString(domain, kFullscreenSyncKey, on ? "on" : "off");
}

void saveNr(bool on, const std::string &domain)
{
	writeString(domain, kNrKey, on ? "on" : "off");
}

void saveNrNetwork(NrNetwork network, const std::string &domain)
{
	writeString(domain, kNrNetworkKey, toString(network));
}

void saveNrHeight(int height, const std::string &domain)
{
	writeString(domain, kNrHeightKey, std::to_string(height).c_str());
}

void saveNrStrength(int percent, const std::string &domain)
{
	writeString(domain, kNrStrengthKey, std::to_string(percent).c_str());
}

void saveNrLook(NrLook look, const std::string &domain)
{
	writeString(domain, kNrLookKey, toString(look));
}

void saveNrTiming(NrTiming timing, const std::string &domain)
{
	writeString(domain, kNrTimingKey, toString(timing));
}

void saveResolution(ChiakiVideoResolutionPreset resolution, const std::string &domain)
{
	writeString(domain, kResolutionKey, resolutionName(resolution));
}

void saveBitrate(unsigned kbps, const std::string &domain)
{
	writeString(domain, kBitrateKey, std::to_string(kbps).c_str());
}

void saveFrameGeneration(bool on, const std::string &domain)
{
	writeString(domain, kFrameGenerationKey, on ? "on" : "off");
}

void saveCameraWarp(bool on, const std::string &domain)
{
	writeString(domain, kCameraWarpKey, on ? "on" : "off");
}

void saveFrameGenerationFactor(int factor, const std::string &domain)
{
	writeString(domain, kFrameGenerationFactorKey, factor == 4 ? "4" : "2");
}

void saveFpsOverlay(bool on, const std::string &domain)
{
	writeString(domain, kFpsOverlayKey, on ? "on" : "off");
}

void savePanelBeside(bool beside, const std::string &domain)
{
	writeString(domain, kPanelPictureKey, beside ? "beside" : "under");
}

void forgetSavedChoices(const std::string &domain)
{
	for(const char *key : { kScalerKey, kSdrColorKey, kHdrOutputKey, kHdrKey, kResolutionKey, kBitrateKey, kHdrBrightnessKey, kHdrToneMapKey,
				kFullscreenSyncKey, kNrKey, kNrNetworkKey, kNrHeightKey, kNrStrengthKey, kNrLookKey, kNrTimingKey, kCameraWarpKey, kFrameGenerationKey, kFrameGenerationFactorKey, kFpsOverlayKey,
				kPanelPictureKey })
		writeString(domain, key, nullptr);
}

std::string applySavedChoices(const SavedChoices &saved, Options &o)
{
	std::string taken;
	auto note = [&taken](const std::string &what) { taken += (taken.empty() ? "" : ", ") + what; };
	if(!o.resolution && saved.resolution)
	{
		o.resolution = saved.resolution;
		note(std::string("stream resolution ") + resolutionName(*saved.resolution));
	}
	if(!o.bitrate_kbps && saved.bitrate_kbps)
	{
		o.bitrate_kbps = saved.bitrate_kbps;
		note("stream bitrate " + std::to_string(*saved.bitrate_kbps) + " kbit/s");
	}
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
	if(!o.fullscreen_sync && saved.fullscreen_sync)
	{
		o.fullscreen_sync = saved.fullscreen_sync;
		note(std::string("fullscreen sync ") + (*saved.fullscreen_sync ? "on" : "off"));
	}
	if(!o.hdr_tone_map && saved.hdr_tone_map)
	{
		o.hdr_tone_map = saved.hdr_tone_map;
		note(std::string("HDR tone mapping ") + video::toString(*saved.hdr_tone_map));
	}
	if(!o.nr && saved.nr)
	{
		o.nr = saved.nr;
		note(*saved.nr ? "neural rendering on" : "neural rendering off");
	}
	if(!o.nr_network && saved.nr_network)
	{
		o.nr_network = saved.nr_network;
		note(std::string("neural rendering network ") + toString(*saved.nr_network));
	}
	if(!o.nr_height && saved.nr_height)
	{
		o.nr_height = saved.nr_height;
		note("neural rendering height " + std::to_string(*saved.nr_height));
	}
	if(!o.nr_strength && saved.nr_strength)
	{
		o.nr_strength = saved.nr_strength;
		note("neural rendering strength " + std::to_string(*saved.nr_strength) + " %");
	}
	if(!o.nr_look && saved.nr_look)
	{
		o.nr_look = saved.nr_look;
		note(std::string("neural rendering look ") + toString(*saved.nr_look));
	}
	if(!o.nr_timing && saved.nr_timing)
	{
		o.nr_timing = saved.nr_timing;
		note(std::string("neural rendering timing ") + toString(*saved.nr_timing));
	}
	if(!o.camera_warp && saved.camera_warp)
	{
		o.camera_warp = saved.camera_warp;
		note(*saved.camera_warp ? "camera warp on" : "camera warp off");
	}
	if(!o.frame_generation_factor && saved.frame_generation_factor)
	{
		o.frame_generation_factor = saved.frame_generation_factor;
		note("frame generation " + std::to_string(*saved.frame_generation_factor) + "x");
	}
	if(!o.frame_generation && saved.frame_generation)
	{
		o.frame_generation = saved.frame_generation;
		note(*saved.frame_generation ? "frame generation on" : "frame generation off");
	}
	if(!o.fps_overlay && saved.fps_overlay)
	{
		o.fps_overlay = saved.fps_overlay;
		note(*saved.fps_overlay ? "frame rate overlay on" : "frame rate overlay off");
	}
	if(!o.panel_beside && saved.panel_beside)
	{
		o.panel_beside = saved.panel_beside;
		note(*saved.panel_beside ? "picture beside the panel" : "picture under the panel");
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
