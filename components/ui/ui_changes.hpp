#pragma once

#include "lvgl.h"

// Changes screen (the Mode::Changes view): where the user tells the model what
// physically changed about the setup — a grinder recalibration (a hard
// calibration *epoch*) or a new bag of beans (a soft quality *marker*). Each tap
// records a boundary timestamp via the calibration component; bucketing the shot
// log into epochs happens at read time, so a change dated back to when it
// actually happened correctly re-buckets shots already on disk.
//
// Like the other panels, this module owns only its look + interaction with the
// calibration store + its fade set. Navigation (Back → Menu) and the mode swap
// stay in ui_report, which injects the Back handler.
namespace espressopost::ui::changes_screen {

// Build the screen under `scr`. Back's CLICKED is wired to `on_back`; the
// date-stepper and the two log pills are handled internally (they touch only the
// calibration store, not navigation). Returns the group handle for the swap.
lv_obj_t* build(lv_obj_t* scr, lv_event_cb_t on_back);

// Repaint the on-record summary from the calibration store. Called at the swap
// midpoint each time the screen is entered so it reflects the latest list.
void refresh();

// The screen's fade set for the section-swap engine.
lv_obj_t* const* fade_widgets(int* out_n);

}  // namespace espressopost::ui::changes_screen
