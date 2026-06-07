#pragma once

#include "lvgl.h"

// Menu screen (the Mode::Menu view): a "MENU" title over a short list of entry
// pills (Presets, Connections, Changes) plus a Back pill. The idle Menu button
// opens this hub; each entry navigates to a sub-screen. Like ui_presets, this
// module owns only the screen's look + its fade set — mode/navigation/swap stays
// in ui_report, which injects the click handlers.
namespace espressopost::ui::menu_screen {

// Build the screen under `scr`. Back's CLICKED is wired to `on_back`; the
// Presets / Connections / Changes entries to `on_presets` / `on_connections` /
// `on_changes`. Returns the group handle so ui_report can show/hide it across
// the mode swap.
lv_obj_t* build(lv_obj_t* scr, lv_event_cb_t on_back,
                lv_event_cb_t on_presets, lv_event_cb_t on_connections,
                lv_event_cb_t on_changes);

// The screen's fade set (title + entries + Back) for the section-swap engine.
lv_obj_t* const* fade_widgets(int* out_n);

}  // namespace espressopost::ui::menu_screen
