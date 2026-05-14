#pragma once

namespace espressopost::ui {

// Build and show the Step-1 bringup screen: a centered number adjustable
// by a touch arc around the rim of the round display. This screen is a
// hardware-verification milestone; it will be replaced when the real idle
// screen lands later in the build order.
//
// Must be called after `display::init()` + `touch::init()`. Acquires the
// LVGL lock internally.
void start_bringup();

}  // namespace espressopost::ui
