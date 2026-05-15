#pragma once

#include "esp_err.h"

namespace espressopost::power {

// Start the idle watchdog. Polls (via an LVGL timer) every 500 ms and
// transitions display state:
//
//   active  -> dimmed   after 30 s of no input
//   dimmed  -> off      after 2 min total of no input
//
// Any call to `consume_input()` resets the elapsed timer. Wake from `off`
// also installs a 500 ms input-debounce window so the wake-up tap doesn't
// register on whatever widget happens to be under the user's finger.
//
// Must be called after `display::init()`. Safe to call once; returns
// ESP_ERR_INVALID_STATE on a second call.
esp_err_t init();

// Notify the power state machine that a user input has occurred (a touch,
// typically). Resets the idle timer and, if the display was dim/off,
// restores it to full brightness.
//
// Returns true if the event should be SUPPRESSED — i.e., we just woke
// from `off`, or we're still inside the post-wake debounce window. The
// touch driver should drop the LVGL event in that case so the wake-up tap
// doesn't ghost-press an invisible widget. Returns false in all other
// states (the event should be forwarded to LVGL normally).
//
// Must be called from the LVGL task (the same task that runs the touch
// read callback) — no internal locking.
bool consume_input();

}  // namespace espressopost::power
