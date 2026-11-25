// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "SavedChoices.h"
#include "TestHarness.h"

#include <CoreFoundation/CoreFoundation.h>

using namespace plk;
using namespace plk::app;

namespace {

// A preferences domain of the tests' own: the app's choices are never touched.
const char *const kTestDomain = "fi.madekivi.pleikkari.mac.tests";

void writeRaw(const char *key, const char *value)
{
	CFStringRef d = CFStringCreateWithCString(nullptr, kTestDomain, kCFStringEncodingUTF8);
	CFStringRef k = CFStringCreateWithCString(nullptr, key, kCFStringEncodingUTF8);
	CFStringRef v = CFStringCreateWithCString(nullptr, value, kCFStringEncodingUTF8);
	CFPreferencesSetAppValue(k, v, d);
	CFPreferencesAppSynchronize(d);
	CFRelease(v);
	CFRelease(k);
	CFRelease(d);
}

} // namespace

PLK_TEST(saved_choices_round_trip)
{
	forgetSavedChoices(kTestDomain);
	SavedChoices c = loadSavedChoices(kTestDomain);
	CHECK(!c.scaler && !c.sdr_color && !c.hdr_output && !c.hdr);

	saveScaler(video::Scaler::Lanczos, kTestDomain);
	saveSdrColor(video::SdrColor::Vivid, kTestDomain);
	saveHdrOutput(video::HdrOutput::ExtendedLinear, kTestDomain);
	saveHdr(true, kTestDomain);
	c = loadSavedChoices(kTestDomain);
	REQUIRE(c.scaler && c.sdr_color && c.hdr_output && c.hdr);
	CHECK(*c.scaler == video::Scaler::Lanczos);
	CHECK(*c.sdr_color == video::SdrColor::Vivid);
	CHECK(*c.hdr_output == video::HdrOutput::ExtendedLinear);
	CHECK(*c.hdr);

	saveHdr(false, kTestDomain);
	saveScaler(video::Scaler::Bilinear, kTestDomain);
	c = loadSavedChoices(kTestDomain);
	REQUIRE(c.scaler && c.hdr);
	CHECK(*c.scaler == video::Scaler::Bilinear);
	CHECK(!*c.hdr);

	// A value this version does not know is left unset, not guessed.
	writeRaw("scaler", "nearest");
	writeRaw("hdr", "maybe");
	c = loadSavedChoices(kTestDomain);
	CHECK(!c.scaler && !c.hdr);
	CHECK(c.sdr_color.has_value());

	forgetSavedChoices(kTestDomain);
	c = loadSavedChoices(kTestDomain);
	CHECK(!c.scaler && !c.sdr_color && !c.hdr_output && !c.hdr);
}

PLK_TEST(saved_choices_fill_only_what_the_command_line_left_open)
{
	SavedChoices saved;
	saved.scaler = video::Scaler::Lanczos;
	saved.sdr_color = video::SdrColor::Vivid;
	saved.hdr_output = video::HdrOutput::ExtendedLinear;
	saved.hdr = true;

	Options o;
	std::string note = applySavedChoices(saved, o);
	REQUIRE(o.scaler && o.sdr_color && o.hdr_output && o.hdr);
	CHECK(*o.scaler == video::Scaler::Lanczos);
	CHECK(*o.sdr_color == video::SdrColor::Vivid);
	CHECK(*o.hdr_output == video::HdrOutput::ExtendedLinear);
	CHECK(*o.hdr);
	CHECK(note == "scaler lanczos, colour vivid, HDR output linear, HDR on");
	CHECK(resolveStreamConfig(o, nullptr).session.codec == CHIAKI_CODEC_H265_HDR);

	// What the command line says stays; --codec decides HDR as much as --hdr does.
	Options given;
	given.scaler = video::Scaler::Bicubic;
	given.sdr_color = video::SdrColor::Accurate;
	given.hdr_output = video::HdrOutput::PQ;
	given.codec = CHIAKI_CODEC_H265;
	note = applySavedChoices(saved, given);
	CHECK(note.empty());
	CHECK(*given.scaler == video::Scaler::Bicubic);
	CHECK(*given.sdr_color == video::SdrColor::Accurate);
	CHECK(*given.hdr_output == video::HdrOutput::PQ);
	CHECK(!given.hdr);
	CHECK(resolveStreamConfig(given, nullptr).session.codec == CHIAKI_CODEC_H265);

	Options nothing;
	CHECK(applySavedChoices(SavedChoices(), nothing).empty());
	CHECK(!nothing.scaler && !nothing.hdr);
}

PLK_TEST(saved_choices_hdr_brightness)
{
	forgetSavedChoices(kTestDomain);
	CHECK(!loadSavedChoices(kTestDomain).hdr_brightness);
	saveHdrBrightness(60, kTestDomain);
	SavedChoices c = loadSavedChoices(kTestDomain);
	REQUIRE(c.hdr_brightness);
	CHECK(*c.hdr_brightness == 60);

	// Used where the command line says nothing, not over what it says.
	Options o;
	CHECK(applySavedChoices(c, o) == "HDR brightness 60 %");
	REQUIRE(o.hdr_brightness);
	CHECK(*o.hdr_brightness == 60);
	Options given;
	given.hdr_brightness = 120;
	CHECK(applySavedChoices(c, given).empty());
	CHECK(*given.hdr_brightness == 120);

	forgetSavedChoices(kTestDomain);
	CHECK(!loadSavedChoices(kTestDomain).hdr_brightness);
}
