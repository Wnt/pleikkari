// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// The pipeline panel: everything the Video menu changes, laid out as the pipeline it
// changes. What is in it is worked out here, from the run's state, as plain data; the
// view (PipelinePanelView.h) draws that and reports clicks.
//
//   pages             Quality, Colour, Display: a tab each
//   processing order  the stages a frame goes through, in order, the ones that do nothing
//                     to this picture struck out; a click shows the stage's card
//   cards             one per stage: a title, one line about what it is doing now (with
//                     its cost where one is measured), a switch where the stage can be
//                     off, and its settings as rows
//   rows              a choice between a few named values, a switch, a value stepped up
//                     and down, or a note
//
// A row's action is a command and a number, the same pair a Video menu item sends (its
// action and its tag), with the menu's wording of the choice for the notice over the
// picture. The tables of named choices below are the menu's own: it is built from them.
#ifndef PLK_APP_PIPELINEPANEL_H
#define PLK_APP_PIPELINEPANEL_H

#include "FrameAnalysis.h"
#include "Runner.h"

#include <string>
#include <vector>

namespace plk::app {

/** One choice of the Video menu: its wording there, a short name for a button, its tag and its key. */
struct NamedChoice
{
	const char *title;
	const char *brief;
	int tag;
	const char *key;
};

extern const NamedChoice kScalerChoices[4];
extern const NamedChoice kSdrColorChoices[2];
extern const NamedChoice kHdrOutputChoices[2];
extern const NamedChoice kNrNetworkChoices[2];
extern const NamedChoice kNrHeightChoices[4];
extern const NamedChoice kNrLookChoices[4];
extern const NamedChoice kNrTimingChoices[3];
extern const NamedChoice kResolutionChoices[4];
extern const NamedChoice kBitrateChoices[5];
extern const NamedChoice kFrameGenerationChoices[2];

enum class PanelCommand
{
	None,
	SelectScaler,
	SelectSdrColor,
	SelectHdrOutput,
	StepHdrBrightness,   // tag +1, -1, or 0 for 100 %
	ToggleHdrToneMap,
	ToggleFullscreenSync,
	ToggleHdr,
	ToggleNr,
	SelectNrNetwork,
	SelectNrHeight,
	StepNrStrength,      // tag +1, -1, or 0 for 100 %
	SelectNrLook,
	ToggleNrCompare,
	SelectNrTiming,
	ToggleFrameGeneration,
	ToggleCameraWarp,
	SelectFrameGenerationFactor,  // tag 2 or 4
	SelectBitrate,       // tag: kbit/s; reconnects
	SelectResolution,    // tag: a ChiakiVideoResolutionPreset; reconnects
	ToggleAnalysis,
	ToggleFpsOverlay,
	SelectPanelPicture,  // tag 1: beside the panel, 0: under it
};

struct PanelAction
{
	PanelCommand command = PanelCommand::None;
	int tag = 0;
	std::string title;  // the Video menu's wording of the choice, where it has one
};

struct PanelOption
{
	std::string title;
	PanelAction action;
	bool selected = false;
	bool enabled = true;
};

struct PanelRow
{
	enum class Kind { Choice, Toggle, Stepper, Note };
	Kind kind = Kind::Note;
	std::string title, detail;
	bool enabled = true;
	std::vector<PanelOption> options;  // Choice
	bool on = false;                   // Toggle
	PanelAction action;                // Toggle
	std::string value;                 // Stepper: the value as text
	PanelAction less, more, reset;     // Stepper
	bool can_less = true, can_more = true, at_default = true;
	bool warning = false;              // Note: something the picture suffers from
};

struct PanelCard
{
	std::string id, title, subtitle, badge;
	std::string icon;  // an SF Symbol's name
	bool has_toggle = false, on = false, toggle_enabled = true;
	PanelAction toggle;
	bool dimmed = false;  // it does nothing to the picture as it is now
	std::vector<PanelRow> rows;
};

struct PanelPage
{
	std::string title;
	std::vector<PanelCard> cards;
	std::vector<PanelAction> reset;  // what brings the page's settings back to their defaults; empty: they are
};

/** A stage in the processing order. */
struct PanelStage
{
	std::string title;
	bool active = true;
	int page = 0;
	std::string card;
};

struct PanelModel
{
	std::vector<PanelPage> pages;
	std::vector<PanelStage> order;
	std::string footer;  // the sizes a frame goes through
	bool warning = false;
};

/** What the panel is made from. */
struct PanelState
{
	Runner::ViewState view;
	AnalysisReport report;
	bool picture_hdr = false;     // the picture being shown is HDR
	bool analysis_shown = false;  // the analysis panel is open
};

PanelModel buildPanel(const PanelState &state);

/** Where a card is: its page, or -1. */
int pageOfCard(const PanelModel &model, const std::string &card);

} // namespace plk::app

#endif // PLK_APP_PIPELINEPANEL_H
