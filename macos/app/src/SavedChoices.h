// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// What was last picked in the Video menu, kept in the app's own preferences
// (defaults read fi.madekivi.pleikkari.mac): scaler, SDR colour, HDR on or off, HDR output,
// HDR brightness.
// A run uses a saved choice only where its command line gives no option.

#ifndef PLK_APP_SAVEDCHOICES_H
#define PLK_APP_SAVEDCHOICES_H

#include "Options.h"

#include <optional>
#include <string>

namespace plk::app {

/** The app's preferences domain: its bundle identifier. */
extern const char *const kChoicesDomain;

struct SavedChoices
{
	std::optional<video::Scaler> scaler;
	std::optional<video::SdrColor> sdr_color;
	std::optional<video::HdrOutput> hdr_output;
	std::optional<bool> hdr;
	std::optional<int> hdr_brightness; // percent
};

/** A value that is missing or not one of the known names is left unset. */
SavedChoices loadSavedChoices(const std::string &domain = kChoicesDomain);

void saveScaler(video::Scaler scaler, const std::string &domain = kChoicesDomain);
void saveSdrColor(video::SdrColor color, const std::string &domain = kChoicesDomain);
void saveHdrOutput(video::HdrOutput output, const std::string &domain = kChoicesDomain);
void saveHdr(bool on, const std::string &domain = kChoicesDomain);
void saveHdrBrightness(int percent, const std::string &domain = kChoicesDomain);
/** Removes every value. */
void forgetSavedChoices(const std::string &domain = kChoicesDomain);

/**
 * Fills in what the command line left open.
 * @return what was taken, for the log ("scaler lanczos, HDR on"); empty if nothing
 */
std::string applySavedChoices(const SavedChoices &saved, Options &options);

} // namespace plk::app

#endif // PLK_APP_SAVEDCHOICES_H
