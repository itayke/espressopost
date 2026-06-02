#pragma once

#include <cstdint>

#include "lvgl.h"
#include "presets.hpp"

// Preset editor (the Mode::Edit view): a "PRESET N" title over three
// steppers (Weight In / Weight Out / Brew Time) flanked by two columns of color
// swatches, with ✕ Cancel / Save › at the bottom. Owns its look, the swatch
// palette, and its form state — but NOT mode / navigation / swap, which ui_report
// drives (it injects the Cancel/Save handlers and reads the fade set). Dependency
// is one-directional: ui_report → ui_preset_edit → ui_stepper.
namespace espressopost::ui::preset_edit {

// Build the screen under `scr`. Cancel's CLICKED → `on_cancel`, Save's →
// `on_save`. Returns the group handle so ui_report can show/hide it across the
// mode swap.
lv_obj_t* build(lv_obj_t* scr, lv_event_cb_t on_cancel, lv_event_cb_t on_save);

// Reseed the form for editing slot `id`: existing values if the slot is active,
// defaults if it's empty. Sets the "PRESET N" title and the Save gate.
// Call just before the screen fades in.
void load(uint8_t slot);

// Compose the edited preset into `*out` (dose/yield/time/color; grind_anchor +
// name preserved from the loaded slot, or defaulted for a new one). Returns
// false when not saveable yet — no color chosen or brew time still "--".
bool gather(presets::Preset* out);

// The screen's fade set (title + bars + captions + stepper + swatches + buttons)
// for the section-swap engine. Stable internal array; *out_n gets the count.
lv_obj_t* const* fade_widgets(int* out_n);

}  // namespace espressopost::ui::preset_edit
