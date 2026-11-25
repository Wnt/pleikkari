// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "PipelinePanel.h"

#include <cstdio>

namespace plk::app {

using video::HdrOutput;
using video::HdrToneMap;
using video::Scaler;
using video::SdrColor;

const NamedChoice kScalerChoices[4] = {
	{ "Bilinear (fastest)", "Bilinear", (int)Scaler::Bilinear, "1" },
	{ "Bicubic", "Bicubic", (int)Scaler::Bicubic, "2" },
	{ "Lanczos (sharpest)", "Lanczos", (int)Scaler::Lanczos, "3" },
	{ "MetalFX upscaler", "MetalFX", (int)Scaler::MetalFX, "4" },
};

const NamedChoice kSdrColorChoices[2] = {
	{ "Accurate", "Accurate", (int)SdrColor::Accurate, "a" },
	{ "Vivid (the display's full gamut)", "Vivid", (int)SdrColor::Vivid, "v" },
};

const NamedChoice kHdrOutputChoices[2] = {
	{ "Output as PQ", "PQ", (int)HdrOutput::PQ, "p" },
	{ "Output as linear light", "Linear light", (int)HdrOutput::ExtendedLinear, "l" },
};

const NamedChoice kNrNetworkChoices[2] = {
	{ "Performance model (small network)", "Performance", (int)NrNetwork::Small, "5" },
	{ "Original model (HT network, slower)", "Original (HT)", (int)NrNetwork::HT, "6" },
};

const NamedChoice kNrHeightChoices[3] = {
	{ "Process at 540p (fastest)", "540p", kNrHeights[0], "7" },
	{ "Process at 720p", "720p", kNrHeights[1], "8" },
	{ "Process at the stream's size (slowest)", "Stream", kNrHeights[2], "9" },
};

const NamedChoice kNrLookChoices[4] = {
	{ "Look: standard", "Standard", (int)NrLook::Standard, "" },
	{ "Look: natural (gentler)", "Natural", (int)NrLook::Natural, "" },
	{ "Look: cinematic", "Cinematic", (int)NrLook::Cinematic, "" },
	{ "Look: neutral (diagnostic)", "Neutral", (int)NrLook::Neutral, "" },
};

namespace {

PanelAction action(PanelCommand command, int tag = 0, const char *title = "")
{
	PanelAction a;
	a.command = command;
	a.tag = tag;
	a.title = title;
	return a;
}

template <size_t N>
PanelRow choiceRow(const char *title, const char *detail, PanelCommand command, const NamedChoice (&choices)[N], int selected)
{
	PanelRow row;
	row.kind = PanelRow::Kind::Choice;
	row.title = title;
	row.detail = detail;
	for(const NamedChoice &c : choices)
	{
		PanelOption o;
		o.title = c.brief;
		o.action = action(command, c.tag, c.title);
		o.selected = c.tag == selected;
		row.options.push_back(o);
	}
	return row;
}

PanelRow toggleRow(const char *title, const char *detail, PanelCommand command, bool on)
{
	PanelRow row;
	row.kind = PanelRow::Kind::Toggle;
	row.title = title;
	row.detail = detail;
	row.on = on;
	row.action = action(command);
	return row;
}

PanelRow stepperRow(const char *title, const char *detail, PanelCommand command, int percent, int standard, bool can_less,
		bool can_more)
{
	PanelRow row;
	row.kind = PanelRow::Kind::Stepper;
	row.title = title;
	row.detail = detail;
	row.value = std::to_string(percent) + " %";
	row.less = action(command, -1);
	row.more = action(command, 1);
	row.reset = action(command, 0);
	row.can_less = can_less;
	row.can_more = can_more;
	row.at_default = percent == standard;
	return row;
}

PanelRow noteRow(const std::string &text, bool warning = false)
{
	PanelRow row;
	row.kind = PanelRow::Kind::Note;
	row.detail = text;
	row.warning = warning;
	return row;
}

std::string size(int width, int height)
{
	return std::to_string(width) + "\xC3\x97" + std::to_string(height);  // a multiplication sign
}

const char *brief(const NamedChoice *choices, size_t count, int tag)
{
	for(size_t i = 0; i < count; i++)
		if(choices[i].tag == tag)
			return choices[i].brief;
	return "";
}

std::string milliseconds(double ms)
{
	char buf[32];
	snprintf(buf, sizeof(buf), ms >= 100 ? "%.0f ms" : ms >= 10 ? "%.1f ms" : "%.2f ms", ms);
	return buf;
}

const char *const kDot = " \xC2\xB7 ";  // a middle dot between the parts of a line

PanelCard neuralRendering(const PanelState &state)
{
	const Runner::ViewState &v = state.view;
	const AnalysisSnapshot &t = state.report.timing;
	const SignalInfo &info = state.report.info;
	const bool working = v.nr && v.nr_available && !v.nr_loading;
	PanelCard card;
	card.id = "nr";
	card.title = "Neural rendering";
	card.icon = "wand.and.stars";
	card.badge = "Experimental";
	card.has_toggle = true;
	card.on = v.nr;
	card.toggle = action(PanelCommand::ToggleNr);
	char buf[160];
	if(!v.nr_available)
		card.subtitle = "Not installed";
	else if(!v.nr)
		card.subtitle = "Off";
	else if(v.nr_loading)
		card.subtitle = "Loading the model";
	else
	{
		// In the manner of an effect layer's line: the size, the strength, what a result takes.
		const int height = info.nr_height > 0 ? info.nr_height : v.nr_height > 0 ? v.nr_height : info.stream_height;
		if(height > 0)
			card.subtitle = std::to_string(height) + "p" + kDot;
		snprintf(buf, sizeof(buf), "strength %.2f", v.nr_strength / 100.0);
		card.subtitle += buf;
		if(t.nr_network.samples)
			card.subtitle += kDot + milliseconds(t.nr_network.mean_ms) + " a result";
		else
			card.subtitle += std::string(kDot) + "no result yet";
	}
	if(!v.nr_available)
		card.rows.push_back(noteRow(v.nr_status, true));
	card.rows.push_back(choiceRow("Model", "The two networks of NVIDIA's model. Performance is the small one, about twice as fast.",
			PanelCommand::SelectNrNetwork, kNrNetworkChoices, (int)v.nr_network));
	card.rows.push_back(choiceRow("Processing size", "The height of the picture the network is given. A result's cost grows with the pixels.",
			PanelCommand::SelectNrHeight, kNrHeightChoices, v.nr_height));
	card.rows.push_back(stepperRow("Strength", "How much of the network's change is shown; above 100 % it is exaggerated. A click on the number: 100 %.",
			PanelCommand::StepNrStrength, v.nr_strength, kNrStrengthDefault, stepNrStrength(v.nr_strength, -1) != v.nr_strength,
			stepNrStrength(v.nr_strength, 1) != v.nr_strength));
	card.rows.push_back(choiceRow("Look", "The rendering profile the network is asked for. Neutral leaves its changes out: a diagnostic.",
			PanelCommand::SelectNrLook, kNrLookChoices, (int)v.nr_look));
	card.rows.push_back(toggleRow("Compare", "The left half of the picture stays as decoded.", PanelCommand::ToggleNrCompare, v.nr_compare));
	if(working)
	{
		// What the mix is, said where it is switched on: a result is not made for the frame
		// it is shown with.
		if(t.nr_age.samples)
			snprintf(buf, sizeof(buf), "A result is %.0f ms old when it is shown, and is laid where the picture was then: a "
					"moving picture ghosts, a still one does not.", t.nr_age.mean_ms);
		else
			snprintf(buf, sizeof(buf), "A result is made from an older frame and laid where the picture was then: a moving "
					"picture ghosts, a still one does not.");
		card.rows.push_back(noteRow(buf, true));
		if(state.picture_hdr)
			card.rows.push_back(noteRow("Of an HDR picture the network is given an SDR rendition; its change is made to the HDR "
					"picture in proportion."));
	}
	return card;
}

PanelCard scaling(const PanelState &state)
{
	const Runner::ViewState &v = state.view;
	const SignalInfo &info = state.report.info;
	const AnalysisSnapshot &t = state.report.timing;
	PanelCard card;
	card.id = "scaling";
	card.title = "Scaling";
	card.icon = "arrow.up.left.and.arrow.down.right";
	card.subtitle = brief(kScalerChoices, 4, (int)v.scaler);
	if(info.stream_width > 0 && info.picture_width > 0)
		card.subtitle += kDot + size(info.stream_width, info.stream_height) + " to " + size(info.picture_width, info.picture_height);
	if(t.stage[AnalysisSnapshot::Gpu].samples)
		card.subtitle += kDot + milliseconds(t.stage[AnalysisSnapshot::Gpu].mean_ms) + " GPU";
	card.rows.push_back(choiceRow("Algorithm", "How the stream's picture is brought to the size it is shown at. Bilinear is the "
			"cheapest; MetalFX is Apple's upscaler.", PanelCommand::SelectScaler, kScalerChoices, (int)v.scaler));
	// The pipeline says what it drew with: MetalFX only enlarges.
	if(v.scaler == Scaler::MetalFX && !info.scaler.empty() && info.scaler != video::toString(Scaler::MetalFX))
		card.rows.push_back(noteRow("MetalFX only enlarges. This picture is shown no larger than the stream sends it, so it is "
				"drawn " + info.scaler + ".", true));
	return card;
}

PanelCard hdrStream(const PanelState &state)
{
	const Runner::ViewState &v = state.view;
	PanelCard card;
	card.id = "hdr-stream";
	card.title = "HDR stream";
	card.icon = "dot.radiowaves.left.and.right";
	card.has_toggle = true;
	card.on = v.hdr_switchable ? v.hdr : state.picture_hdr;
	card.toggle_enabled = v.hdr_switchable;
	card.toggle = action(PanelCommand::ToggleHdr);
	card.subtitle = !v.hdr_switchable ? (state.picture_hdr ? "HDR10; switched in a stream of a PS5 only" : "SDR; switched in a stream of a PS5 only")
			: v.hdr ? "The console is asked for HDR10" : "The console is asked for SDR";
	card.rows.push_back(noteRow("The console fixes this when a session starts: switching reconnects, about ten seconds without "
			"picture."));
	return card;
}

PanelCard hdrPicture(const PanelState &state)
{
	const Runner::ViewState &v = state.view;
	PanelCard card;
	card.id = "hdr-picture";
	card.title = "HDR picture";
	card.icon = "sun.max";
	card.dimmed = !state.picture_hdr;
	if(!state.picture_hdr)
		card.subtitle = "For an HDR picture; this one is SDR";
	else
		card.subtitle = std::string(brief(kHdrOutputChoices, 2, (int)v.hdr_output)) + kDot + "brightness " + std::to_string(v.hdr_brightness)
				+ " %" + kDot + (v.hdr_tone_map == HdrToneMap::System ? "fitted by macOS" : "fitted by the app");
	card.rows.push_back(choiceRow("Output", "What the display is handed: the stream's own PQ code values, or linear light.",
			PanelCommand::SelectHdrOutput, kHdrOutputChoices, (int)v.hdr_output));
	card.rows.push_back(stepperRow("Brightness", "A factor on the picture's light. At 100 % HDR reference white is as bright as the "
			"display's SDR white. A click on the number: 100 %.", PanelCommand::StepHdrBrightness, v.hdr_brightness, kHdrBrightnessDefault,
			stepHdrBrightness(v.hdr_brightness, -1) != v.hdr_brightness, stepHdrBrightness(v.hdr_brightness, 1) != v.hdr_brightness));
	card.rows.push_back(toggleRow("Tone mapping by macOS", "Off: the app fits the picture to the display, and a fullscreen picture "
			"goes straight to it. On: macOS does, and a fullscreen picture stalls.", PanelCommand::ToggleHdrToneMap,
			v.hdr_tone_map == HdrToneMap::System));
	return card;
}

PanelCard sdrColour(const PanelState &state)
{
	const Runner::ViewState &v = state.view;
	PanelCard card;
	card.id = "sdr-colour";
	card.title = "SDR colour";
	card.icon = "paintpalette";
	card.dimmed = state.picture_hdr;
	card.subtitle = state.picture_hdr ? "For an SDR picture; this one is HDR" : brief(kSdrColorChoices, 2, (int)v.sdr_color);
	card.rows.push_back(choiceRow("Colour", "Accurate shows the console's colours. Vivid takes them as the display's own, more "
			"saturated ones.", PanelCommand::SelectSdrColor, kSdrColorChoices, (int)v.sdr_color));
	return card;
}

PanelCard fullscreen(const PanelState &state)
{
	const Runner::ViewState &v = state.view;
	PanelCard card;
	card.id = "fullscreen";
	card.title = "In step with the display";
	card.icon = "display";
	card.has_toggle = true;
	card.on = v.fullscreen_sync;
	card.toggle_enabled = v.fullscreen_sync_applies;
	card.toggle = action(PanelCommand::ToggleFullscreenSync);
	card.subtitle = !v.fullscreen_sync_applies ? "The pacing is fixed by --pacing"
			: v.fullscreen_sync ? "Fullscreen: no tearing, about 5 ms slower" : "Fullscreen: shown at once, can tear";
	return card;
}

PanelCard fpsOverlay(const PanelState &state)
{
	PanelCard card;
	card.id = "fps-overlay";
	card.title = "Frame rate overlay";
	card.icon = "speedometer";
	card.has_toggle = true;
	card.on = state.view.fps_overlay;
	card.toggle = action(PanelCommand::ToggleFpsOverlay);
	card.subtitle = "FPS and frame times over the picture's corner";
	return card;
}

PanelCard analysis(const PanelState &state)
{
	PanelCard card;
	card.id = "analysis";
	card.title = "Frame time analysis";
	card.icon = "chart.bar.xaxis";
	card.has_toggle = true;
	card.on = state.analysis_shown;
	card.toggle = action(PanelCommand::ToggleAnalysis);
	card.subtitle = "What each stage of a frame costs, under the picture";
	return card;
}

PanelCard thisPanel(const PanelState &state)
{
	PanelCard card;
	card.id = "panel";
	card.title = "This panel";
	card.icon = "sidebar.right";
	card.subtitle = state.view.panel_beside ? "The picture is fitted beside it" : "It lies over the picture's edge";
	PanelRow row;
	row.kind = PanelRow::Kind::Choice;
	row.title = "Picture";
	row.detail = "Beside: the whole picture, smaller. Under: the picture keeps the size it is played at, which is the size to "
			"judge the scaling at.";
	PanelOption beside, under;
	beside.title = "Beside";
	beside.action = action(PanelCommand::SelectPanelPicture, 1, "Picture beside the panel");
	beside.selected = state.view.panel_beside;
	under.title = "Under";
	under.action = action(PanelCommand::SelectPanelPicture, 0, "Picture under the panel");
	under.selected = !state.view.panel_beside;
	row.options = { beside, under };
	card.rows.push_back(row);
	return card;
}

} // namespace

PanelModel buildPanel(const PanelState &state)
{
	const Runner::ViewState &v = state.view;
	const SignalInfo &info = state.report.info;
	PanelModel m;
	m.warning = state.report.warning;

	PanelPage quality;
	quality.title = "Quality";
	quality.cards = { neuralRendering(state), scaling(state) };
	if(!v.nr && v.nr_available)
		quality.reset.push_back(action(PanelCommand::ToggleNr));
	if(v.nr_network != NrNetwork::Small)
		quality.reset.push_back(action(PanelCommand::SelectNrNetwork, (int)NrNetwork::Small, kNrNetworkChoices[0].title));
	if(v.nr_height != kNrHeightDefault)
		quality.reset.push_back(action(PanelCommand::SelectNrHeight, kNrHeightDefault, kNrHeightChoices[1].title));
	if(v.nr_strength != kNrStrengthDefault)
		quality.reset.push_back(action(PanelCommand::StepNrStrength, 0));
	if(v.nr_look != NrLook::Standard)
		quality.reset.push_back(action(PanelCommand::SelectNrLook, (int)NrLook::Standard, kNrLookChoices[0].title));
	if(v.nr_compare)
		quality.reset.push_back(action(PanelCommand::ToggleNrCompare));
	if(v.scaler != Scaler::Bilinear)
		quality.reset.push_back(action(PanelCommand::SelectScaler, (int)Scaler::Bilinear, kScalerChoices[0].title));

	// Not the HDR stream itself: that one reconnects.
	PanelPage colour;
	colour.title = "Colour";
	colour.cards = { hdrStream(state), hdrPicture(state), sdrColour(state) };
	if(v.hdr_output != HdrOutput::PQ)
		colour.reset.push_back(action(PanelCommand::SelectHdrOutput, (int)HdrOutput::PQ, kHdrOutputChoices[0].title));
	if(v.hdr_brightness != kHdrBrightnessDefault)
		colour.reset.push_back(action(PanelCommand::StepHdrBrightness, 0));
	if(v.hdr_tone_map != HdrToneMap::App)
		colour.reset.push_back(action(PanelCommand::ToggleHdrToneMap));
	if(v.sdr_color != SdrColor::Accurate)
		colour.reset.push_back(action(PanelCommand::SelectSdrColor, (int)SdrColor::Accurate, kSdrColorChoices[0].title));

	PanelPage display;
	display.title = "Display";
	display.cards = { fullscreen(state), fpsOverlay(state), analysis(state), thisPanel(state) };
	if(!v.fullscreen_sync && v.fullscreen_sync_applies)
		display.reset.push_back(action(PanelCommand::ToggleFullscreenSync));
	if(!v.fps_overlay)
		display.reset.push_back(action(PanelCommand::ToggleFpsOverlay));
	if(!v.panel_beside)
		display.reset.push_back(action(PanelCommand::SelectPanelPicture, 1, "Picture beside the panel"));
	m.pages = { quality, colour, display };

	// The order a frame goes through them. The mix, the fitting of an HDR picture and the
	// scaler are one pass over the frame (two with MetalFX); vivid colour is the display
	// taking the result's code values as its own primaries.
	const bool nr = v.nr && v.nr_available;
	auto stage = [&m](const std::string &title, bool active, const char *card) {
		PanelStage s;
		s.title = title;
		s.active = active;
		s.card = card;
		s.page = pageOfCard(m, card);
		m.order.push_back(s);
	};
	stage(state.picture_hdr ? "Input" + std::string(kDot) + "HDR" : "Input", true, "hdr-stream");
	stage("Neural rendering", nr, "nr");
	stage("HDR", state.picture_hdr, "hdr-picture");
	stage(std::string("Scaling") + kDot + brief(kScalerChoices, 4, (int)v.scaler), true, "scaling");
	stage(std::string("Colour") + kDot + "vivid", !state.picture_hdr && v.sdr_color == SdrColor::Vivid, "sdr-colour");
	stage("Display", true, "fullscreen");

	if(info.stream_width > 0)
	{
		m.footer = "Input " + size(info.stream_width, info.stream_height);
		if(nr && info.nr_width > 0)
			m.footer += " / NR " + size(info.nr_width, info.nr_height);
		if(info.picture_width > 0)
			m.footer += " / output " + size(info.picture_width, info.picture_height);
		m.footer += nr ? std::string(kDot) + "mixed in the scaler's pass, the network beside the stream" : "";
	}
	else
		m.footer = "Waiting for the first frame";
	return m;
}

int pageOfCard(const PanelModel &model, const std::string &card)
{
	for(size_t p = 0; p < model.pages.size(); p++)
		for(const PanelCard &c : model.pages[p].cards)
			if(c.id == card)
				return (int)p;
	return -1;
}

} // namespace plk::app
