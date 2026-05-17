#pragma once

#include <cstdint>

#include "esp_err.h"

namespace espressopost::model {

// What we hand back to the UI / submit path.
//
// `grind`              — absolute grind setting the model recommends to land
//                        time_delta=0 at the current climate; NaN when the
//                        preset doesn't have enough useful data yet.
// `confidence_pct`     — 0..100 in 5-unit steps. 0 means "do not display" —
//                        the caller should suppress the suggestion entirely
//                        (either fall back to UI placeholder or hide the row).
//                        The mapping is heuristic on top of the Bayesian
//                        posterior predictive variance; calibration is fine
//                        for v1 since the indicator is meant to be directional
//                        ("the model is starting to learn") not probabilistic.
struct Suggestion {
  float   grind;
  uint8_t confidence_pct;
};

// Build the per-preset fits from whatever's currently in the shot log. Safe to
// call once at boot, after storage::init() and presets::init() have returned.
// Allocates an internal mutex; no other resources.
esp_err_t init();

// Re-read /littlefs/shots.bin and refit every preset. Cheap on the data
// volumes we expect (tens to hundreds of shots, 40 B each); intentionally
// synchronous so the caller can rely on `suggest_for_preset` returning a fresh
// answer immediately afterward. Call this after every successful
// storage::append_shot so the next preset cycle reflects the new data point.
void refit();

// Current best suggestion for `preset_id` given the latest climate reading.
// Always returns something — but `confidence_pct == 0` means "don't trust this
// number, show nothing". Reasons that can yield 0:
//   - preset has fewer shots than the floor needed for a stable fit
//   - all training shots used the same grind (no slope to extrapolate from)
//   - posterior predictive variance is above the display threshold
//   - climate sensor hasn't reported yet (timestamp_us == 0)
Suggestion suggest_for_preset(uint8_t preset_id);

}  // namespace espressopost::model
