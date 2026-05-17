// Host-side unit tests for components/model/model_math.{hpp,cpp}. Build with
// tests/host/run.sh (cmake + ninja). The math layer has no IDF deps so this
// binary is plain C++17, runs in milliseconds, and gets us a tight feedback
// loop for tuning priors / weights / confidence mapping without flashing.
//
// Tests are deliberately behavioral — they assert on directions, magnitudes,
// and monotonic properties rather than exact floats. That lets us tune the
// math (phantom magnitudes, kPriorPrec, confidence scale) without rewriting
// the test bodies every time.

#define CATCH_CONFIG_MAIN
#include "catch.hpp"

#include "model_math.hpp"

#include <cmath>
#include <vector>

using namespace espressopost::model;

namespace {

// Typical at-the-bench climate. Matches the phantoms' pinned values so the
// suggestion in "no real climate trend" tests resolves around mean grind.
constexpr ClimateInput kTypicalClimate = {22.0f, 50.0f, 1013.0f};

// Deterministic, repeatable pseudo-random — same seed → same shots, so a
// failure is reproducible. Plain LCG; quality is fine for a unit test.
struct Rng {
  unsigned x;
  explicit Rng(unsigned seed) : x(seed) {}
  float next01() {
    x = x * 1103515245u + 12345u;
    return ((x >> 16) & 0x7FFF) / 32767.0f;
  }
  float uniform(float lo, float hi) { return lo + next01() * (hi - lo); }
};

// Synthesize N shots that obey time_delta = β_g_real·(grind - g_center)
// + climate terms + noise. Grind is sampled uniformly in [g_center -
// g_spread, g_center + g_spread], climate stays constant per call.
std::vector<FitSample> synth_grind_slope(int n,
                                         float g_center, float g_spread,
                                         float beta_g_real,
                                         ClimateInput climate,
                                         float noise_amp = 1.0f,
                                         unsigned seed = 1) {
  Rng rng(seed);
  std::vector<FitSample> out;
  out.reserve(n);
  for (int i = 0; i < n; ++i) {
    const float g = rng.uniform(g_center - g_spread, g_center + g_spread);
    const float noise = (rng.next01() - 0.5f) * 2.0f * noise_amp;
    const float y = beta_g_real * (g - g_center) + noise;
    out.push_back({g, climate.temp_c, climate.humidity_pct,
                   climate.pressure_hpa, y});
  }
  return out;
}

// De-standardize a fitted β_g back to grinder-dial units (∂y/∂g in real
// space). Used by tests that want to compare against the synthetic slope.
float real_grind_slope(const PresetFit& f) {
  return f.beta[0] * f.std_y / f.std_g;
}

}  // namespace

// -----------------------------------------------------------------------------
// fit() — basic shape / preconditions
// -----------------------------------------------------------------------------

TEST_CASE("zero real shots → invalid fit", "[fit]") {
  PresetFit f = fit(nullptr, 0);
  REQUIRE_FALSE(f.valid);
  REQUIRE(f.n_used == 0);
}

TEST_CASE("one real shot is below floor → invalid fit", "[fit]") {
  FitSample s = {5.2f, 22.0f, 50.0f, 1013.0f, +1.0f};
  PresetFit f = fit(&s, 1);
  REQUIRE_FALSE(f.valid);
}

TEST_CASE("two real shots cross the floor and produce a fit", "[fit]") {
  FitSample shots[] = {
      {5.2f, 22.0f, 50.0f, 1013.0f, +2.0f},
      {5.2f, 22.0f, 50.0f, 1013.0f, -1.0f},
  };
  PresetFit f = fit(shots, 2);
  REQUIRE(f.valid);
  REQUIRE(f.n_used == 2);
  // All stds finite and above the kStdFloor, even though real shots show no
  // grind variance — phantoms inject it.
  REQUIRE(std::isfinite(f.std_g));
  REQUIRE(f.std_g > 1e-3f);
}

// -----------------------------------------------------------------------------
// Phantom prior — direction + std injection
// -----------------------------------------------------------------------------

TEST_CASE("phantom prior gives β_g the right sign when real shots are uninformative",
          "[fit][phantoms]") {
  // 5 real shots, all at the same grind. Without phantoms the data has no
  // grind→time slope to learn; with phantoms (finer→longer) β_g should land
  // negative in standardized space.
  std::vector<FitSample> shots(5,
      {5.2f, 22.0f, 50.0f, 1013.0f, +1.0f});
  PresetFit f = fit(shots.data(), shots.size());
  REQUIRE(f.valid);
  REQUIRE(f.beta[0] < 0.0f);
}

TEST_CASE("phantom prior is weak — real slope dominates with ~20 shots",
          "[fit][phantoms]") {
  // Real data carries a clear slope OPPOSITE to the phantom direction
  // (positive β_g, "finer → shorter" — backwards-grinder simulation). With
  // enough real data the fitted β_g should flip positive despite the prior.
  auto shots = synth_grind_slope(/*n=*/20, /*g_center=*/5.5f, /*spread=*/1.0f,
                                 /*beta_g_real=*/+2.0f, kTypicalClimate);
  PresetFit f = fit(shots.data(), shots.size());
  REQUIRE(f.valid);
  REQUIRE(real_grind_slope(f) > 0.0f);  // data won
}

// -----------------------------------------------------------------------------
// Recovery of a known slope
// -----------------------------------------------------------------------------

TEST_CASE("clear synthetic slope is recovered within tolerance", "[fit]") {
  // True relationship: time_delta = -2.0 · (grind - 5.5) + small noise.
  auto shots = synth_grind_slope(/*n=*/30, 5.5f, 1.0f, -2.0f, kTypicalClimate,
                                 /*noise=*/0.5f);
  PresetFit f = fit(shots.data(), shots.size());
  REQUIRE(f.valid);
  REQUIRE(real_grind_slope(f) < -1.0f);
  REQUIRE(real_grind_slope(f) > -3.0f);  // wide band; tightens as data grows
}

// -----------------------------------------------------------------------------
// suggest() — direction + climate sensitivity
// -----------------------------------------------------------------------------

TEST_CASE("suggestion lands inside the trained grind range", "[suggest]") {
  // Time_delta mean ≈ 0 by construction → suggested grind sits near g_center.
  auto shots = synth_grind_slope(/*n=*/30, 5.5f, 1.0f, -2.0f, kTypicalClimate,
                                 /*noise=*/0.5f);
  PresetFit f = fit(shots.data(), shots.size());
  Suggestion s = suggest(f, kTypicalClimate);
  REQUIRE(s.confidence_pct > 0);
  REQUIRE(s.grind > 4.5f);
  REQUIRE(s.grind < 6.5f);
}

TEST_CASE("climate trend in training data shifts the suggested grind",
          "[suggest][climate]") {
  // Build shots where time_delta depends on BOTH grind AND temp:
  //   y = -2·(g - 5.5) - 0.5·(T - 23)
  // → hotter climate makes shots run shorter → to land time_delta=0 the
  //   model should suggest FINER grind (lower number) at higher T.
  Rng rng(42);
  std::vector<FitSample> shots;
  for (int i = 0; i < 40; ++i) {
    const float g = rng.uniform(4.5f, 6.5f);
    const float T = rng.uniform(20.0f, 26.0f);
    const float y = -2.0f * (g - 5.5f) - 0.5f * (T - 23.0f)
                  + (rng.next01() - 0.5f);
    shots.push_back({g, T, 50.0f, 1013.0f, y});
  }
  PresetFit f = fit(shots.data(), shots.size());
  REQUIRE(f.valid);

  Suggestion hot  = suggest(f, {26.0f, 50.0f, 1013.0f});
  Suggestion cold = suggest(f, {20.0f, 50.0f, 1013.0f});
  REQUIRE(hot.confidence_pct > 0);
  REQUIRE(cold.confidence_pct > 0);
  // Hot → finer (lower grind) than cold.
  REQUIRE(hot.grind < cold.grind);
}

TEST_CASE("invalid fit yields suppress-suggestion", "[suggest]") {
  PresetFit f = fit(nullptr, 0);  // invalid
  Suggestion s = suggest(f, kTypicalClimate);
  REQUIRE(s.confidence_pct == 0);
  REQUIRE(std::isnan(s.grind));
}

// -----------------------------------------------------------------------------
// Confidence behavior
// -----------------------------------------------------------------------------

TEST_CASE("confidence is rounded to 5% steps", "[confidence]") {
  // Sample many fits across different data sizes; every nonzero confidence
  // value reported should be a multiple of 5.
  for (int n = 2; n <= 50; n += 3) {
    auto shots = synth_grind_slope(n, 5.5f, 1.0f, -2.0f, kTypicalClimate,
                                   /*noise=*/0.5f, /*seed=*/static_cast<unsigned>(n));
    PresetFit f = fit(shots.data(), shots.size());
    Suggestion s = suggest(f, kTypicalClimate);
    INFO("n=" << n << " conf=" << static_cast<int>(s.confidence_pct));
    REQUIRE(s.confidence_pct % 5 == 0);
  }
}

TEST_CASE("confidence never exceeds the 95% display cap", "[confidence]") {
  // Pile on data — even with 200 perfectly-consistent shots, the cap should
  // hold so the UI never reads "100%".
  auto shots = synth_grind_slope(/*n=*/200, 5.5f, 1.0f, -2.0f, kTypicalClimate,
                                 /*noise=*/0.1f);
  PresetFit f = fit(shots.data(), shots.size());
  Suggestion s = suggest(f, kTypicalClimate);
  REQUIRE(s.confidence_pct <= 95);
}

TEST_CASE("confidence is independent of climate query point (uses Σ[0,0])",
          "[confidence]") {
  // Confidence is now driven by the marginal variance of β_g (Σ[0,0]) — a
  // property of the fit alone, not of the query point. So evaluating the
  // same fit at wildly different climates must produce the same confidence
  // number. (Predictive-variance confidence would NOT pass this — V_param
  // grows quadratically as the query moves away from the centroid.)
  Rng rng(11);
  std::vector<FitSample> shots;
  for (int i = 0; i < 30; ++i) {
    const float g = rng.uniform(4.5f, 6.5f);
    const float T = rng.uniform(20.0f, 26.0f);
    const float y = -2.0f * (g - 5.5f) - 0.5f * (T - 23.0f)
                  + (rng.next01() - 0.5f);
    shots.push_back({g, T, 50.0f, 1013.0f, y});
  }
  PresetFit f = fit(shots.data(), shots.size());
  REQUIRE(f.valid);

  const uint8_t centroid = suggest(f, {23.0f, 50.0f, 1013.0f}).confidence_pct;
  const uint8_t hot      = suggest(f, {26.0f, 50.0f, 1013.0f}).confidence_pct;
  const uint8_t cold     = suggest(f, {20.0f, 50.0f, 1013.0f}).confidence_pct;
  INFO("centroid=" << static_cast<int>(centroid)
       << " hot=" << static_cast<int>(hot)
       << " cold=" << static_cast<int>(cold));
  REQUIRE(centroid == hot);
  REQUIRE(centroid == cold);
}

TEST_CASE("confidence does not decrease as more data arrives",
          "[confidence]") {
  // Use the climate-varying generator + an off-center climate query so
  // V_param at the query point is genuinely non-zero. (With constant-climate
  // training and a centroid query, V_param ≈ 0 and confidence saturates at
  // the 95% cap on the first valid fit — accurate math behavior, but not
  // a discriminating test.)
  auto pull_conf = [](int n) -> int {
    Rng rng(7);
    std::vector<FitSample> shots;
    for (int i = 0; i < n; ++i) {
      const float g = rng.uniform(4.5f, 6.5f);
      const float T = rng.uniform(20.0f, 26.0f);
      const float y = -2.0f * (g - 5.5f) - 0.5f * (T - 23.0f)
                    + (rng.next01() - 0.5f);
      shots.push_back({g, T, 50.0f, 1013.0f, y});
    }
    PresetFit f = fit(shots.data(), shots.size());
    if (!f.valid) return 0;
    return suggest(f, {26.0f, 50.0f, 1013.0f}).confidence_pct;
  };
  const int c5  = pull_conf(5);
  const int c20 = pull_conf(20);
  const int c80 = pull_conf(80);
  INFO("c5=" << c5 << " c20=" << c20 << " c80=" << c80);
  // Monotonic non-decrease is the property we actually want — adding data
  // to a Bayesian posterior never widens its variance in expectation.
  // Magnitude jumps depend on noise + saturation and would over-fit the test
  // to the current confidence-mapping constants.
  REQUIRE(c20 >= c5);
  REQUIRE(c80 >= c20);
}
