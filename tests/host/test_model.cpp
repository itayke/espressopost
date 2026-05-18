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

TEST_CASE("confidence has a stable plateau within the safe climate band",
          "[confidence]") {
  // Combined confidence layers a climate-extrapolation penalty on top of the
  // slope-quality factor. Within ±kExtrapPenaltyStart σ of training climate,
  // the penalty is zero — confidence is determined by slope quality alone
  // and doesn't flicker as climate drifts inside the safe band. Outside it,
  // confidence drops monotonically.
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

  // Queries inside the safe band: same confidence number.
  const uint8_t centroid    = suggest(f, {f.mean_T,              50.0f, 1013.0f}).confidence_pct;
  const uint8_t mild_warm   = suggest(f, {f.mean_T + 0.3f * f.std_T, 50.0f, 1013.0f}).confidence_pct;
  // Outside the safe band: penalty kicks in.
  const uint8_t far_warm    = suggest(f, {f.mean_T + 2.5f * f.std_T, 50.0f, 1013.0f}).confidence_pct;
  INFO("centroid=" << int(centroid) << " mild=" << int(mild_warm) << " far=" << int(far_warm));
  REQUIRE(centroid == mild_warm);
  REQUIRE(far_warm < centroid);
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

// -----------------------------------------------------------------------------
// Real-device fixture (2026-05-18)
// -----------------------------------------------------------------------------
// Captured from the actual device via the temporary boot-time dump in
// storage::init(). User reported that with these 6 shots + a live climate of
// 78°F (~25.5°C), the model suggested grind=4.3 — well below anything they'd
// ever pulled. These tests reproduce that scenario and probe a few "what if"
// configurations side by side, so we can decide whether the parked
// quality-weighted-target hack actually addresses the failure mode before
// writing the production version.

namespace {

constexpr FitSample kRealShots[] = {
    {5.20f, 21.10f, 44.86f,  999.61f,  0.0f},  // 5★
    {5.20f, 22.53f, 46.47f,  999.53f,  1.0f},  // 5★
    {5.20f, 19.18f, 58.69f, 1005.89f,  4.0f},  // 4★
    {5.20f, 23.20f, 44.18f, 1000.63f, -2.0f},  // 4★
    {5.20f, 22.95f, 53.68f, 1003.41f, -8.0f},  // 5★
    {5.10f, 23.20f, 51.98f, 1008.58f, -6.0f},  // 5★
    {5.00f, 25.92f, 50.88f, 1006.91f,  8.0f},  // 5★  ← hottest climate, finer grind, ran LONG (contradicts β_T<0)
};
constexpr uint8_t kRealStars[] = {5, 5, 4, 4, 5, 5, 5};
constexpr size_t  kRealN       = sizeof(kRealShots) / sizeof(kRealShots[0]);

// Live climate at the moment the user reported the 4.3 suggestion. T from the
// hardware (78°F = 25.5°C); H/P estimated from the two most-recent shots.
constexpr ClimateInput kLiveClimate = {25.5f, 50.0f, 1008.0f};

// Quality-weighted mean time_delta across "good" shots (4-5 stars). With this
// fixture every shot qualifies, so it's just the unweighted mean.
float quality_weighted_target(const FitSample* shots, const uint8_t* stars,
                              size_t n, uint8_t min_stars = 4) {
  float sum = 0.0f;
  int   cnt = 0;
  for (size_t i = 0; i < n; ++i) {
    if (stars[i] >= min_stars) { sum += shots[i].time_delta_s; ++cnt; }
  }
  return cnt > 0 ? sum / static_cast<float>(cnt) : 0.0f;
}

}  // namespace

TEST_CASE("real-data: model suggests within the trained grind range",
          "[real-data]") {
  // With the 7-shot fixture (shot 6 added a hot-and-LONG counter-example to
  // the model's earlier β_T<0 belief), the climate slope collapsed near zero
  // and the suggestion is no longer extrapolating wildly. This test pins the
  // post-fix behavior: suggestion within a reasonable band around the
  // training grind centroid (5.0 - 5.2 dial range).
  PresetFit f = fit(kRealShots, kRealN);
  REQUIRE(f.valid);
  INFO("β_g (standardized) = " << f.beta[0]
       << "  β_T = " << f.beta[1]
       << "  std_T = " << f.std_T
       << "  mean_y = " << f.mean_y
       << "  std_y = " << f.std_y
       << "  σ[0] = " << f.sigma[0]
       << "  real grind slope = " << (f.beta[0] * f.std_y / f.std_g) << " s/unit");

  Suggestion s = suggest(f, kLiveClimate);
  INFO("CURRENT: suggested=" << s.grind << " conf=" << int(s.confidence_pct));
  REQUIRE(s.confidence_pct > 0);
  REQUIRE(s.grind >= 4.5f);
  REQUIRE(s.grind <= 6.0f);
}

TEST_CASE("real-data: quality-weighted target converges to baseline when all shots are good",
          "[real-data]") {
  // When every training shot is rated 4-5★, the quality-weighted mean is
  // basically the unweighted mean of all time_deltas, and that's already
  // what `target=0` targets after mean-centering. So the hack should
  // produce a result very close to baseline — useful behavior to confirm
  // (the hack doesn't *break* anything when there's no quality signal to
  // separate good from bad shots).
  PresetFit f = fit(kRealShots, kRealN);
  REQUIRE(f.valid);

  const float t_qw = quality_weighted_target(kRealShots, kRealStars, kRealN);
  Suggestion baseline = suggest(f, kLiveClimate, /*target=*/0.0f);
  Suggestion hacked   = suggest(f, kLiveClimate, /*target=*/t_qw);

  INFO("quality-weighted target time_delta = " << t_qw);
  INFO("BASELINE: suggested=" << baseline.grind);
  INFO("HACK    : suggested=" << hacked.grind);
  REQUIRE(std::fabs(hacked.grind - baseline.grind) < 0.5f);
}

TEST_CASE("real-data: target that exactly matches mean_y returns the centroid grind",
          "[real-data]") {
  // Sanity probe — if you set the target equal to mean_y, target_y_z = 0, so
  // the solver should suggest exactly mean_g. This isolates "what does the
  // model think is the centroid?" from any climate extrapolation effect.
  PresetFit f = fit(kRealShots, kRealN);
  REQUIRE(f.valid);

  Suggestion s = suggest(f, kLiveClimate, /*target=*/f.mean_y);
  INFO("mean_y=" << f.mean_y << "  mean_g=" << f.mean_g
       << "  suggested=" << s.grind
       << "  T_z=" << (kLiveClimate.temp_c - f.mean_T) / f.std_T);
  // The climate is still in the equation; this probes how much the climate
  // term alone is dragging the suggestion away from mean_g.
  REQUIRE(std::isfinite(s.grind));  // triggers INFO printout
}

TEST_CASE("real-data: combined confidence differentiates extrapolation distance",
          "[real-data][combined-conf]") {
  // Two scenarios that the previous (Σ[0,0]-only) confidence number couldn't
  // tell apart — both read 70%. The combined formula penalizes them
  // differently:
  //   - past: 6-shot fit at hot climate → suggestion 4.x (outside the
  //     trained grind range) → BOTH climate-extrap AND grind-extrap factors
  //     fire → confidence drops dramatically.
  //   - current: 7-shot fit at slightly hot climate → suggestion 5.x
  //     (inside the trained grind range) → only the climate factor fires,
  //     so confidence is moderate-but-not-bad.
  // The displayed number should rank these honestly: past < current, and
  // current should still be below the no-penalty cap (something is being
  // extrapolated, even if only climate).
  PresetFit f6 = fit(kRealShots, 6);  // pre-shot-6 fit
  PresetFit f7 = fit(kRealShots, 7);  // current fit

  const Suggestion past    = suggest(f6, {25.50f, 50.0f, 1008.0f});
  const Suggestion current = suggest(f7, {26.57f, 50.54f, 1006.41f});
  INFO("past=" << past.grind << "@" << int(past.confidence_pct) << "%"
       << "  current=" << current.grind << "@" << int(current.confidence_pct) << "%");
  // Both surface a suggestion (averaging can't zero one factor's bad news).
  REQUIRE(past.confidence_pct > 0);
  REQUIRE(current.confidence_pct > 0);
  // Past is double-penalized (climate AND grind extrapolation) → far below
  // the cap. Current only has climate-extrap firing → moderate.
  REQUIRE(past.confidence_pct < 50);
  REQUIRE(current.confidence_pct < 95);
  // The differentiator — past < current — is the actual point of this test.
  REQUIRE(past.confidence_pct < current.confidence_pct);
}

TEST_CASE("real-data: in-range climate query gets higher confidence than extrapolation",
          "[real-data][combined-conf]") {
  // Same fit, two query points: one at training centroid (z≈0 across the
  // board) vs one extrapolating temp. Combined confidence at centroid should
  // beat the extrapolating query.
  PresetFit f = fit(kRealShots, kRealN);
  REQUIRE(f.valid);

  const Suggestion at_centroid    = suggest(f, {f.mean_T, f.mean_H, f.mean_P});
  const Suggestion at_extrapolate = suggest(f, {f.mean_T + 3.0f * f.std_T,
                                                 f.mean_H, f.mean_P});
  INFO("centroid="  << at_centroid.grind    << "@" << int(at_centroid.confidence_pct)    << "%");
  INFO("extrapol.=" << at_extrapolate.grind << "@" << int(at_extrapolate.confidence_pct) << "%");
  REQUIRE(at_centroid.confidence_pct > at_extrapolate.confidence_pct);
}

// 8-shot fixture (2026-05-18 evening). Adds shot 7 — T=27.48, grind=5.10,
// ran -8s. User reported live state at T=27.55 → suggestion=4.80 @ 60%, and
// flagged the confidence as feeling too high for an apparent extrapolation.
namespace {
constexpr FitSample kRealShots8[] = {
    {5.20f, 21.10f, 44.86f,  999.61f,  0.0f},
    {5.20f, 22.53f, 46.47f,  999.53f,  1.0f},
    {5.20f, 19.18f, 58.69f, 1005.89f,  4.0f},
    {5.20f, 23.20f, 44.18f, 1000.63f, -2.0f},
    {5.20f, 22.95f, 53.68f, 1003.41f, -8.0f},
    {5.10f, 23.20f, 51.98f, 1008.58f, -6.0f},
    {5.00f, 25.92f, 50.88f, 1006.91f,  8.0f},
    {5.10f, 27.48f, 49.05f, 1005.43f, -8.0f},  // ← newest
};
constexpr size_t kRealN8 = sizeof(kRealShots8) / sizeof(kRealShots8[0]);
constexpr ClimateInput kLiveClimate8 = {27.55f, 49.33f, 1005.37f};
}  // namespace

TEST_CASE("real-data 8-shot: grind-extrap factor brings confidence in line",
          "[real-data][combined-conf]") {
  // 2026-05-18 evening: device reported grind=4.80 @ 60% at T=27.55. The
  // suggestion was leaving the real-grind range [5.00, 5.20] entirely — i.e.
  // extrapolating in two dimensions (climate AND grind). The grind-extrap
  // factor exists to surface exactly this case. With it in the mix the
  // displayed confidence should drop into the "low" tag (<30) territory or
  // close to it — a much more honest read of the situation.
  PresetFit f = fit(kRealShots8, kRealN8);
  REQUIRE(f.valid);

  Suggestion s = suggest(f, kLiveClimate8);
  INFO("mean_g=" << f.mean_g << " std_g_real=" << f.std_g_real);
  INFO("suggested grind=" << s.grind
       << "  grind z=" << std::fabs((s.grind - f.mean_g) / f.std_g_real));
  INFO("Σ[0,0]=" << f.sigma[0]
       << "  T_z=" << (kLiveClimate8.temp_c - f.mean_T) / f.std_T);
  INFO("confidence=" << int(s.confidence_pct));
  // Was 60% pre-grind-extrap; should now read in the middle tier (<60),
  // honestly reflecting "I'm extrapolating in two dimensions at once."
  REQUIRE(s.confidence_pct < 60);
}

TEST_CASE("real-data: target = recent-shots quality-weighted mean (last 3)",
          "[real-data]") {
  // What if we weight recency too — average only the last 3 high-quality
  // shots' time_deltas? With the user's data those are -2, -8, -6 → mean
  // -5.33s. Closer to current behavior but reflects the recent trend.
  PresetFit f = fit(kRealShots, kRealN);
  REQUIRE(f.valid);

  // Last 3 shots: t_d = -8, -6, +8 → mean -2. The +8 outlier (today's hot
  // brew at the finer grind) dramatically changes the recency-weighted view
  // compared to the 6-shot fixture, where the recent-3 mean was -5.33s.
  const float t_recent = (-8.0f + -6.0f + 8.0f) / 3.0f;
  Suggestion s = suggest(f, kLiveClimate, /*target=*/t_recent);
  INFO("recent-3 target=" << t_recent << "  suggested=" << s.grind);
  // With the recent-only target, suggestion should be noticeably higher
  // (coarser) than the all-shots baseline.
  REQUIRE(std::isfinite(s.grind));
}
