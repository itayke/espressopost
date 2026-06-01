#pragma once

#include "lvgl.h"

// Presets screen (the Mode::Presets view): a "PRESETS" title over a 3×3 grid of
// preset slots plus a Back pill. This module owns the screen's look, its
// custom-drawn glyphs, and its grid state — but NOT mode/navigation/swap, which
// ui_report drives (it injects the Back handler and reads the fade set). The
// dependency is one-directional: ui_report → ui_presets → ui_preset_readout.
namespace espressopost::ui::presets_screen {

// Menu pill accent (pill border + "Menu" label + hamburger bars). Exposed
// because the Menu button itself lives on the idle center line, built by
// ui_report; its glyph comes from build_menu_glyph below.
extern const lv_color_t kColorMenu;

// Build the screen under `scr`; the Back pill's CLICKED is wired to `on_back`.
// Returns the group handle so ui_report can show/hide it across the mode swap.
lv_obj_t* build(lv_obj_t* scr, lv_event_cb_t on_back);

// Repopulate the grid from current preset data (active slots show a colored
// readout, empty slots a bare outline). Call just before the screen fades in.
void refresh();

// The screen's fade set (title + 9 slots + Back) for the section-swap engine.
// Returns a stable internal array; *out_n receives the count.
lv_obj_t* const* fade_widgets(int* out_n);

// Build the hamburger glyph widget (sized + painter attached) under `parent` for
// the Menu pill ui_report assembles. Returns the glyph object.
lv_obj_t* build_menu_glyph(lv_obj_t* parent);

}  // namespace espressopost::ui::presets_screen
