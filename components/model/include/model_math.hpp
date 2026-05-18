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

// Minimum feature row the fit needs from one shot. Decoupled from
// storage::ShotRecord to keep the math layer free of storage's (and therefore
// IDF's) includes.
struct FitSample {
  float user_grind;
  float temp_c;
  float humidity_pct;
  float pressure_hpa;
  float time_delta_s;
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
//                  same standardized space β lives in. These include the
//                  synthetic phantom shots — used for the fit math.
// `mean_g_real`,   real-shots-only grind centroid and spread, floored at
// `std_g_real`     kGrindStdFloor. Used by suggest() to score how far the
//                  recommended grind is from anything the user has actually
//                  pulled; phantoms are excluded so the score reflects user
//                  behavior, not the prior.
// `beta[]`         standardized coefficients in [grind, T, H, P] order.
// `sigma[]`        4×4 posterior covariance, row-major. Used for the
//                  predictive parameter variance that drives confidence.
struct PresetFit {
  bool     valid;
  uint16_t n_used;
  float    mean_g, std_g;
  float    mean_T, std_T;
  float    mean_H, std_H;
  float    mean_P, std_P;
  float    mean_y, std_y;
  float    mean_g_real, std_g_real;
  float    beta[4];
  float    sigma[4 * 4];
};

// Fit one preset from `samples` (already filtered: caller has dropped
// tombstones / anomalies / climate-less shots). Returns a PresetFit; `valid`
// is false if there's not enough real data (< 2 samples) or the precision
// matrix went singular (defensive — the +λI ridge prevents this in practice).
//
// Synthetic phantom shots (see model_math.cpp) are always folded in at low
// weight so the math behaves gracefully even when real shots haven't varied
// the grind dial yet.
PresetFit fit(const FitSample* samples, std::size_t n_real);

// Best grind suggestion for a fitted preset evaluated against `climate`.
// Always returns a Suggestion; `confidence_pct == 0` means "suppress, the
// caller should hide the row" (insufficient data, β_g near zero, predictive
// variance above threshold).
//
// `target_time_delta` lets callers override the suggestion solver's
// objective. The default (0) means "solve for the grind that lands the
// preset's stated target time" — current production behavior. Pass a
// non-zero value (e.g. the quality-weighted mean of historical time_deltas)
// to nudge the model toward a different time target without re-fitting; the
// host tests use this to compare the in-product behavior against the
// "quality-weighted target" hack discussed in the parked-suggestion memory.
Suggestion suggest(const PresetFit& f, ClimateInput climate,
                   float target_time_delta = 0.0f);

}  // namespace espressopost::model
