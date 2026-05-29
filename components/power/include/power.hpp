#pragma once

#include "esp_err.h"

namespace espressopost::power {

// Start the idle watchdog. Polls (via an LVGL timer) and transitions
// display state:
//
//   active  -> dimmed   after the active-idle threshold
//   dimmed  -> off      after the further dim-idle threshold
//
// Thresholds live in power.cpp (kIdleToDimUs, kIdleToOffUs); init() logs
// the resolved values at boot.
//
// Any call to `consume_input()` resets the elapsed timer. Wake from `off`
// OR from `dimmed` suppresses the wake-up tap so it doesn't ghost-press
// whatever widget happens to be under the user's finger. Suppression lifts
// at the LATER of (a) the touch driver reporting a release (via
// `note_release()`) and (b) a 500 ms safety floor — the floor catches the
// rare case where the controller never reports a clean release.
//
// Must be called after `display::init()`. Safe to call once; returns
// ESP_ERR_INVALID_STATE on a second call.
esp_err_t init();

// Notify the power state machine that a user input (a touch, typically)
// has occurred. Resets the idle timer and, if the display was dim/off,
// restores it to full brightness.
//
// Returns true if the event should be SUPPRESSED — i.e., we just woke
// from `dim` or `off`, the wake-up tap finger hasn't been observed
// lifting yet, or we're still inside the post-wake 500 ms safety floor.
// The touch driver should drop the LVGL event in that case. Returns false
// in all other states (the event should be forwarded to LVGL normally).
//
// Must be called from the LVGL task (the same task that runs the touch
// read callback) — no internal locking.
bool consume_input();

// Notify the power state machine that no touch was detected this frame —
// i.e., the touch driver's read returned zero points. Lets the suppression
// gate observe the press-to-release edge that follows a wake-tap, so the
// next genuine press can flow through. Cheap no-op when no wake is
// pending. Must be called from the LVGL task.
void note_release();

}  // namespace espressopost::power
