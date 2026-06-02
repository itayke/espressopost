#pragma once

#include <cstdint>

#include "esp_err.h"
#include "model_math.hpp"  // pulls in Suggestion + the rest of the pure-math types

namespace espressopost::storage { struct ShotRecord; }

namespace espressopost::model {

// IDF-bound API. The pure math (fit + suggest, defined in model_math.hpp) is
// what the host unit tests under tests/host/ exercise; the wrapper below adds:
//   - thread-safe singleton state (one PresetFit per preset, mutex-guarded)
//   - reading shots out of LittleFS via storage::read_shots
//   - fetching live climate via climate::latest()
//   - boot/refit logging via esp_log
//
// `Suggestion` itself is defined in model_math.hpp so it can be used from
// both worlds with one definition.

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

// Result of comparing a just-finished shot against what the model predicted for
// it — drives the out-of-band tip on the Report screen. `predicted_time_s` is
// the model's expected brew time in seconds (NaN if there was no usable fit),
// handy for the tip copy ("53s vs ~33s expected").
struct ShotAssessment {
  ShotVerdict verdict;
  float       predicted_time_s;
};

// Assess a just-appended shot against the CURRENT fits. MUST be called BEFORE
// refit(), so the fits still reflect the model that produced the suggestion the
// user saw — the shot is judged against what the model knew *before* it was
// added, not after. Reads the fit for rec.preset_id under the same lock as
// refit(), using the record's own grind + climate + recorded confidence.
// Returns {InBand, NaN} when there's no usable fit or the model wasn't
// confident (see model_math::classify_shot for the gating).
ShotAssessment assess_shot(const storage::ShotRecord& rec);

}  // namespace espressopost::model
