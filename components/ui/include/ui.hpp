#pragma once

namespace espressopost::ui {

// Build and show the Report screen: climate strip at the top, time-delta
// stepper + 1–5 quality stars in the middle, Submit at the bottom. Submit is
// enabled only after the user has explicitly set both fields; tapping it
// appends a ShotRecord to LittleFS and resets the form.
//
// This is the device's primary screen until the idle screen lands in a later
// build-order step.
//
// Must be called after display::init() + touch::init() + storage::init().
// Acquires the LVGL lock internally.
void start_report();

}  // namespace espressopost::ui
