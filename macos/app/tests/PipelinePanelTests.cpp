// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "PipelinePanel.h"
#include "TestHarness.h"

using namespace plk;
using namespace plk::app;

namespace {

// A 1080p SDR stream in a window, everything as a fresh start has it, the engine there.
PanelState state()
{
	PanelState s;
	s.view.nr = true;
	s.view.nr_available = true;
	s.view.fps_overlay = true;
	s.view.hdr_switchable = true;
	s.report.info.stream_width = 1920;
	s.report.info.stream_height = 1080;
	s.report.info.nr_width = 1280;
	s.report.info.nr_height = 720;
	s.report.info.picture_width = 2880;
	s.report.info.picture_height = 1620;
	return s;
}

const PanelCard *card(const PanelModel &m, const char *id)
{
	for(const PanelPage &p : m.pages)
		for(const PanelCard &c : p.cards)
			if(c.id == id)
				return &c;
	return nullptr;
}

const PanelRow *row(const PanelCard &c, const char *title)
{
	for(const PanelRow &r : c.rows)
		if(r.title == title)
			return &r;
	return nullptr;
}

const PanelStage *stage(const PanelModel &m, const char *card_id)
{
	for(const PanelStage &s : m.order)
		if(s.card == card_id)
			return &s;
	return nullptr;
}

// The one option of a choice row that is selected, or -1 when it is not exactly one.
int selectedTag(const PanelRow &r)
{
	int tag = -1, count = 0;
	for(const PanelOption &o : r.options)
		if(o.selected)
		{
			tag = o.action.tag;
			count++;
		}
	return count == 1 ? tag : -1;
}

} // namespace

PLK_TEST(panel_pages_and_cards)
{
	const PanelModel m = buildPanel(state());
	REQUIRE(m.pages.size() == 3);
	CHECK(m.pages[0].title == "Quality");
	CHECK(m.pages[1].title == "Colour");
	CHECK(m.pages[2].title == "Display");
	CHECK(pageOfCard(m, "nr") == 0);
	CHECK(pageOfCard(m, "scaling") == 0);
	CHECK(pageOfCard(m, "hdr-stream") == 1);
	CHECK(pageOfCard(m, "hdr-picture") == 1);
	CHECK(pageOfCard(m, "sdr-colour") == 1);
	CHECK(pageOfCard(m, "fullscreen") == 2);
	CHECK(pageOfCard(m, "fps-overlay") == 2);
	CHECK(pageOfCard(m, "analysis") == 2);
	CHECK(pageOfCard(m, "panel") == 2);
	CHECK(pageOfCard(m, "frame-generation") == -1);
	// Every stage of the order has a card to show.
	for(const PanelStage &s : m.order)
		CHECK(s.page == pageOfCard(m, s.card) && s.page >= 0);
	// A fresh start has nothing to reset.
	for(const PanelPage &p : m.pages)
		CHECK(p.reset.empty());
	CHECK(m.footer.find("Input 1920") == 0);
	CHECK(m.footer.find("NR 1280") != std::string::npos);
	CHECK(m.footer.find("output 2880") != std::string::npos);
}

PLK_TEST(panel_order_follows_the_picture)
{
	PanelState s = state();
	PanelModel m = buildPanel(s);
	REQUIRE(m.order.size() == 6);
	CHECK(m.order.front().title == "Input" && m.order.back().title == "Display");
	CHECK(stage(m, "nr")->active);
	CHECK(!stage(m, "hdr-picture")->active);
	CHECK(stage(m, "scaling")->active);
	CHECK(stage(m, "scaling")->title.find("Bilinear") != std::string::npos);
	CHECK(!stage(m, "sdr-colour")->active);

	// Switched off, or not there to be switched on: not a stage of this picture.
	s.view.nr = false;
	CHECK(!stage(buildPanel(s), "nr")->active);
	s.view.nr = true;
	s.view.nr_available = false;
	m = buildPanel(s);
	CHECK(!stage(m, "nr")->active);
	CHECK(m.footer.find("NR") == std::string::npos);

	s = state();
	s.view.sdr_color = video::SdrColor::Vivid;
	s.view.scaler = video::Scaler::MetalFX;
	m = buildPanel(s);
	CHECK(stage(m, "sdr-colour")->active);
	CHECK(stage(m, "scaling")->title.find("MetalFX") != std::string::npos);

	// An HDR picture: its fitting is a stage, the colour of an SDR picture is not.
	s.picture_hdr = true;
	s.view.hdr = true;
	m = buildPanel(s);
	CHECK(stage(m, "hdr-picture")->active);
	CHECK(!stage(m, "sdr-colour")->active);
	CHECK(m.order.front().title.find("HDR") != std::string::npos);
	CHECK(!card(m, "hdr-picture")->dimmed);
	CHECK(card(m, "sdr-colour")->dimmed);
	s.picture_hdr = false;
	m = buildPanel(s);
	CHECK(card(m, "hdr-picture")->dimmed);
	CHECK(!card(m, "sdr-colour")->dimmed);
}

PLK_TEST(panel_rows_show_the_state)
{
	PanelState s = state();
	s.view.scaler = video::Scaler::Lanczos;
	s.view.nr_network = NrNetwork::HT;
	s.view.nr_height = 540;
	s.view.nr_look = NrLook::Cinematic;
	s.view.nr_strength = kNrStrengthMax;
	s.view.nr_compare = true;
	s.view.hdr_output = video::HdrOutput::ExtendedLinear;
	s.view.hdr_brightness = kHdrBrightnessMin;
	s.view.sdr_color = video::SdrColor::Vivid;
	s.view.panel_beside = false;
	const PanelModel m = buildPanel(s);

	const PanelCard &nr = *card(m, "nr");
	CHECK(nr.has_toggle && nr.on && nr.toggle.command == PanelCommand::ToggleNr);
	CHECK(nr.badge == "Experimental");
	CHECK(selectedTag(*row(nr, "Model")) == (int)NrNetwork::HT);
	CHECK(selectedTag(*row(nr, "Processing size")) == 540);
	CHECK(selectedTag(*row(nr, "Look")) == (int)NrLook::Cinematic);
	const PanelRow &strength = *row(nr, "Strength");
	CHECK(strength.kind == PanelRow::Kind::Stepper);
	CHECK(strength.value == "200 %");
	CHECK(strength.can_less && !strength.can_more && !strength.at_default);
	CHECK(strength.less.command == PanelCommand::StepNrStrength && strength.less.tag == -1);
	CHECK(strength.more.tag == 1 && strength.reset.tag == 0);
	CHECK(row(nr, "Compare")->on);

	CHECK(selectedTag(*row(*card(m, "scaling"), "Algorithm")) == (int)video::Scaler::Lanczos);
	// The option carries the menu's wording, for the notice over the picture.
	bool found = false;
	for(const PanelOption &o : row(*card(m, "scaling"), "Algorithm")->options)
		if(o.action.tag == (int)video::Scaler::Lanczos)
		{
			found = true;
			CHECK(o.title == "Lanczos");
			CHECK(o.action.title == "Lanczos (sharpest)");
			CHECK(o.action.command == PanelCommand::SelectScaler);
		}
	CHECK(found);

	const PanelCard &hdr = *card(m, "hdr-picture");
	CHECK(selectedTag(*row(hdr, "Output")) == (int)video::HdrOutput::ExtendedLinear);
	const PanelRow &brightness = *row(hdr, "Brightness");
	CHECK(brightness.value == "20 %");
	CHECK(!brightness.can_less && brightness.can_more && !brightness.at_default);
	CHECK(selectedTag(*row(*card(m, "sdr-colour"), "Colour")) == (int)video::SdrColor::Vivid);
	CHECK(selectedTag(*row(*card(m, "panel"), "Picture")) == 0);
	CHECK(card(m, "fps-overlay")->on);
	CHECK(!card(m, "analysis")->on);
}

PLK_TEST(panel_reset_brings_back_the_defaults)
{
	PanelState s = state();
	s.view.nr = false;
	s.view.nr_network = NrNetwork::HT;
	s.view.nr_height = 0;
	s.view.nr_strength = 130;
	s.view.nr_look = NrLook::Natural;
	s.view.nr_compare = true;
	s.view.scaler = video::Scaler::Bicubic;
	s.view.hdr = true;  // not something reset touches: it reconnects
	s.view.hdr_output = video::HdrOutput::ExtendedLinear;
	s.view.hdr_brightness = 150;
	s.view.hdr_tone_map = video::HdrToneMap::System;
	s.view.sdr_color = video::SdrColor::Vivid;
	s.view.fullscreen_sync = false;
	s.view.fps_overlay = false;
	s.view.panel_beside = false;
	PanelModel m = buildPanel(s);
	CHECK(m.pages[0].reset.size() == 7);
	CHECK(m.pages[1].reset.size() == 4);
	CHECK(m.pages[2].reset.size() == 3);
	for(const PanelAction &a : m.pages[1].reset)
		CHECK(a.command != PanelCommand::ToggleHdr);
	bool scaler = false;
	for(const PanelAction &a : m.pages[0].reset)
		if(a.command == PanelCommand::SelectScaler)
		{
			scaler = true;
			CHECK(a.tag == (int)video::Scaler::Bilinear);
			CHECK(a.title == "Bilinear (fastest)");
		}
	CHECK(scaler);

	// Neural rendering that is not there is not something to switch back on, and a pacing
	// fixed from the command line is not the panel's to change.
	s.view.nr_available = false;
	s.view.fullscreen_sync_applies = false;
	m = buildPanel(s);
	for(const PanelAction &a : m.pages[0].reset)
		CHECK(a.command != PanelCommand::ToggleNr);
	for(const PanelAction &a : m.pages[2].reset)
		CHECK(a.command != PanelCommand::ToggleFullscreenSync);
	CHECK(!card(m, "fullscreen")->toggle_enabled);
	// Its own switch stays, as the menu's item does: using it says what is missing. The
	// card says it too.
	const PanelCard &nr = *card(m, "nr");
	CHECK(nr.toggle_enabled);
	CHECK(nr.subtitle == "Not installed");
	REQUIRE(!nr.rows.empty());
	CHECK(nr.rows.front().kind == PanelRow::Kind::Note && nr.rows.front().warning);
}

PLK_TEST(panel_says_what_the_picture_suffers_from)
{
	// A result laid over a later frame: the card says how old it is, as a warning.
	PanelState s = state();
	s.report.timing.enhancer_active = true;
	s.report.timing.nr_age.mean_ms = 102;
	s.report.timing.nr_age.samples = 60;
	const PanelModel m = buildPanel(s);
	const PanelCard &nr = *card(m, "nr");
	bool warned = false;
	for(const PanelRow &r : nr.rows)
		if(r.kind == PanelRow::Kind::Note && r.warning)
		{
			warned = true;
			CHECK(r.detail.find("102 ms") != std::string::npos);
		}
	CHECK(warned);
	// No frame yet: the footer says so and no size is made up.
	PanelState none;
	CHECK(buildPanel(none).footer == "Waiting for the first frame");
}

// The Video menu is built from these tables: a tag or a key that changes here changes there.
PLK_TEST(panel_named_choices)
{
	CHECK(kScalerChoices[3].tag == (int)video::Scaler::MetalFX);
	CHECK(std::string(kScalerChoices[3].title) == "MetalFX upscaler");
	CHECK(std::string(kScalerChoices[0].key) == "1" && std::string(kScalerChoices[3].key) == "4");
	CHECK(std::string(kSdrColorChoices[1].key) == "v");
	CHECK(std::string(kHdrOutputChoices[0].key) == "p" && std::string(kHdrOutputChoices[1].key) == "l");
	CHECK(std::string(kNrNetworkChoices[0].key) == "5" && kNrNetworkChoices[1].tag == (int)NrNetwork::HT);
	for(int i = 0; i < 3; i++)
		CHECK(kNrHeightChoices[i].tag == kNrHeights[i]);
	CHECK(std::string(kNrHeightChoices[0].key) == "7" && std::string(kNrHeightChoices[2].key) == "9");
	CHECK(kNrLookChoices[3].tag == (int)NrLook::Neutral);
}
