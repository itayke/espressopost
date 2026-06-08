#pragma once

#include <cstddef>
#include <cstdint>

// Pure math layer for the per-preset Bayesian time model. Intentionally has
// zero ESP-IDF dependencies (no FreeRTOS, no esp_err_t, no esp_log) so it can
// compile and run host-side under any C++17 compiler — see tests/host/ for
// the standalone test binary. The IDF-bound wrapper in model.cpp owns the
// mutex, file I/O, climate fetch, and logging; it converts storage::ShotRecord
// → FitSample, calls into here, and exposes the public model.hpp API.
//
// All public types (FitSample, ClimateInput, PresetFit, Suggestion) are plain
// data so they can be constructed, inspected, and round-tripped from tests
// without any factory ceremony.

namespace espressopost::model {

// Resolution of the grinder dial — single source of truth for "snap to dial
// step" rounding. The UI uses it when finalizing dragged values; the model
// uses it when emitting a suggestion so suggestions land on a setting the
// user can actually dial in. Changing this changes both ends in lockstep.
constexpr float kGrindStep = 0.05f;

// Continuous regression features, fixed order: [grind, T, H, P]. The grind
// coefficient is always beta[0], so the confidence and readback code can index
// it directly no matter how many epoch-offset columns follow.
constexpr int kNCont = 4;

// Calibration epochs (Tier 0). When the grinder is recalibrated or the machine
// swapped, the same dial number means a different grind — so the model fits a
// per-epoch intercept *offset* (βg·(g+δ) = βg·g + βg·δ) that puts pre/post
// shots on one comparable scale, while the climate slopes stay pooled across
// every epoch (the coffee physics didn't change, only the dial did). The
// latest epoch is the regression reference (offset 0), so suggest()/predict()
// always evaluate "as the setup behaves now"; each older epoch with shots earns
// one ridge-shrunk dummy column. Capped so the augmented matrix stays small and
// stack-friendly — far above any realistic count of recals/machine swaps over a
// device's life (the wrapper merges the oldest epochs if somehow exceeded).
constexpr int kMaxEpochs = 8;
constexpr int kMaxFeat   = kNCont + (kMaxEpochs - 1);  // continuous + epoch dummies

// Minimum feature row the fit needs from one shot. Decoupled from
// storage::ShotRecord to keep the math layer free of storage's (and therefore
// IDF's) includes. `actual_time_s` is the raw shot time in seconds — the
// model regresses it against the other features and `suggest()` solves for
// the grind that lands the preset's `target_time_s` (passed in by the
// caller). Storing absolute time keeps the math invariant against later
// preset target changes: only the target plugged into `suggest()` shifts.
//
// `epoch_index` buckets the shot onto the calibration timeline (0 = oldest;
// see calibration::epoch_index). The wrapper fills it; host tests leave it 0
// (single-epoch / pooled), which reproduces the pre-epoch behavior exactly.
struct FitSample {
  float   user_grind;
  float   temp_c;
  float   humidity_pct;
  float   pressure_hpa;
  float   actual_time_s;
  uint8_t epoch_index = 0;  // 0 = oldest / single-epoch; default keeps brace-init callers pooled
};

// Live climate at predict time. The wrapper pulls this from climate::latest();
// tests just construct one inline.
struct ClimateInput {
  float temp_c;
  float humidity_pct;
  float pressure_hpa;
};

// Public result type — same shape as the model.hpp API, defined here so the
// math layer is self-contained and tests can use it without including
// model.hpp (which drags in esp_err.h).
//
// `grind`            absolute grind setting recommended; NaN when no useful
//                    answer (insufficient data, climate missing, β_g ≈ 0, …).
// `confidence_pct`   0..95 in 5-unit steps. 0 = suppress entirely; nonzero =
//                    the wrapper UI should surface this value.
struct Suggestion {
  float   grind;
  uint8_t confidence_pct;
};

// Internal fit state for one preset. Plain data — tests inspect fields
// directly to assert on means / stds / β / Σ. Carry it across calls to
// suggest() as long as you want the same fit; refit by calling fit() again.
//
// `n_used`         number of REAL samples that contributed (phantoms are
//                  always present internally; they don't count here).
// `mean_*, std_*`  per-feature standardization parameters captured at fit
//                  time. suggest() needs them to map live climate into the
//                  same standardized space β lives in. With symmetric
//                  adaptive phantom placement (see model_math.cpp), mean_g
//                  algebraically equals the real-shots-only grind mean —
//                  so it doubles as both the fit's standardization basis
//                  and the user's observed dial centroid.
//                  NOTE: `mean_y`/`std_y` are in LOG-seconds — the model
//                  regresses log(brew time), not raw seconds (flow through a
//                  puck is convex in grind, so time is convex; logging
//                  linearizes it and symmetrizes residuals). The seconds⇄log
//                  transform is internal to model_math: FitSample.actual_time_s
//                  and suggest()'s target_time_s stay in seconds.
// `std_g_real`     real-shots-only grind spread, floored at kGrindStdFloor.
//                  Used by suggest() to score how far the recommended
//                  grind is from anything the user has actually pulled;
//                  phantoms are excluded so the score reflects user
//                  behavior, not the prior.
// `beta[]`         standardized coefficients: [grind, T, H, P] in slots 0..3,
//                  then one per modeled older epoch (centered 0/1 dummy). Only
//                  the first `n_dim` slots are meaningful.
// `sigma[]`        n_dim×n_dim posterior covariance, row-major (stride n_dim).
//                  sigma[0] is β_g's variance, the slope-quality confidence
//                  input, regardless of how many epoch columns trail it.
//
// Per-epoch offset model (Tier 0) — all zero/pooled when n_epochs ≤ 1:
// `n_dim`          active regression dimension = kNCont + (modeled older epochs).
// `n_epochs`       calibration epochs reflected in this fit (1 = no offset).
// `y_z_ref_dummy`  the reference (latest) epoch's dummy contribution to
//                  standardized y. suggest()/predict() fold it in so they
//                  evaluate at the current epoch even though y was centered
//                  across all epochs. 0 when pooled.
// `epoch_grind_offset[]` per-epoch dial offset from the reference epoch, in
//                  grind units (latest = 0; positive = that epoch's dial read
//                  coarser, i.e. the current dial reads finer than it did).
//                  NaN for epochs with no shots. Feeds the Events readback.
// `epoch_n_used[]` real shots contributing per epoch — gates the readback's
//                  "gathering data" copy until an epoch's offset has firmed up.
struct PresetFit {
  bool     valid;
  uint16_t n_used;
  float    mean_g, std_g;
  float    mean_T, std_T;
  float    mean_H, std_H;
  float    mean_P, std_P;
  float    mean_y, std_y;
  float    std_g_real;
  float    beta[kMaxFeat];
  float    sigma[kMaxFeat * kMaxFeat];
  uint8_t  n_dim;
  uint8_t  n_epochs;
  float    y_z_ref_dummy;
  float    epoch_grind_offset[kMaxEpochs];
  uint16_t epoch_n_used[kMaxEpochs];
};

// Fit one preset from `samples` (already filtered: caller has dropped
// tombstones / anomalies / climate-less shots). Returns a PresetFit; `valid`
// is false if there's not enough real data (< 2 samples) or the precision
// matrix went singular (defensive — the +λI ridge prevents this in practice).
//
// Synthetic phantom shots (see model_math.cpp) are always folded in at low
// weight so the math behaves gracefully even when real shots haven't varied
// the grind dial yet.
//
// `n_epochs` is the number of calibration epochs spanning these samples (from
// calibration::epoch_count; the per-sample bucket is FitSample::epoch_index).
// The default of 1 means "single epoch / pooled" — every existing caller and
// host test keeps the exact pre-epoch behavior. With n_epochs ≥ 2 the fit adds
// a per-epoch intercept offset (see kMaxEpochs).
PresetFit fit(const FitSample* samples, std::size_t n_real, uint8_t n_epochs = 1);

// Best grind suggestion for a fitted preset evaluated against `climate`.
// Always returns a Suggestion; `confidence_pct == 0` means "suppress, the
// caller should hide the row" (insufficient data, β_g near zero, predictive
// variance above threshold).
//
// `target_time_s` is the brew time the solver aims for (typically the
// preset's `target_time_s`). With shot records now storing absolute brew
// seconds, this is the only knob that shifts when a user retunes a preset's
// target — β stays invariant and only the standardized target moves. Callers
// that want to override the target with a non-zero alternative (e.g. the
// quality-weighted mean of historical brew times) can pass any positive
// value; host tests do this to compare in-product behavior against the
// "quality-weighted target" hack from the parked-suggestion memory.
Suggestion suggest(const PresetFit& f, ClimateInput climate,
                   float target_time_s);

// Forward evaluation: the brew time (in SECONDS) the fitted model expects for a
// shot pulled at `grind` under `climate`. This is the inverse direction of
// suggest() — suggest() solves grind for a target time, predict_time_s() solves
// time for a given grind. Climate is clamped the same way suggest() clamps it
// (kClimateClampZ) so a wild live reading can't project the slope into a region
// we never sampled. Returns NaN when the fit is invalid.
float predict_time_s(const PresetFit& f, ClimateInput climate, float grind);

// How a finished shot's actual brew time compares to what the model predicted
// for it. `InBand` means "nothing to flag" — used both for genuinely on-target
// shots AND for the cases where we deliberately stay quiet (invalid fit, or the
// model wasn't confident enough to have an opinion worth defending).
enum class ShotVerdict { InBand, RanLong, RanShort };

// Classify a finished shot for the out-of-band tip. Conservative by design:
// returns `InBand` unless the model was confident (confidence_pct >=
// kTipConfidenceGate) AND the actual time deviates from prediction by more than
// kTipBandLogRatio in log space (a symmetric multiplicative margin — same %
// either direction). `RanLong` = slower than predicted, `RanShort` = faster.
// `confidence_pct` is the suggestion confidence recorded with the shot; it
// gates the tip so we never cry wolf on shots the model couldn't predict well.
ShotVerdict classify_shot(const PresetFit& f, ClimateInput climate, float grind,
                          float actual_time_s, uint8_t confidence_pct);

}  // namespace espressopost::model
