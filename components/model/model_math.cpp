#include "model_math.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace espressopost::model {
namespace {

// Time model features after centering/scaling: grind, temp, humidity, pressure.
// (Intercept absorbed by centering y and X.) Spec calls for two models (time
// + quality); v1 ships time-only because it has the cleanest grind→target
// relationship. Quality model + cross-preset pooling are follow-ups, gated
// behind enough data to make extra parameters earn their keep.
constexpr int kNFeat = 4;

// Ridge prior precision λ on each (standardized) coefficient. Spec calls for
// "effective sample size ≈ 3 shots" before data dominates the fit; with
// standardized features that maps directly to λ ≈ 3 in the (X'WX + λI)
// posterior precision. Same value picks up double duty as our confidence
// scale: at zero data, x*ᵀ Σ x* ≈ ‖x*‖² / λ, so unit-length query points top
// out at V_param ≈ 1/λ, which is also our suggestion threshold below.
constexpr float kPriorPrec = 3.0f;

// Synthetic "phantom" shots prepended to every preset's training data. They
// encode the one universal fact about grind→time that we don't want the model
// to have to rediscover from scratch: lower grind number = finer = longer
// pull (assumed convention; flip the time_delta_s signs if a future grinder's
// dial runs the other way). Climate values land at typical room conditions
// so the phantoms barely touch β_T/β_H/β_P after standardization.
//
// Two purposes:
//   1) Inject grind variance — your first dozen shots may all be at the same
//      dial setting, leaving std_g floored and β_g unlearnable. Phantoms fix
//      that on day one.
//   2) Bias β_g to the right sign with non-trivial magnitude, so the very
//      first suggestion is at least pointing the user the right way.
//
// Each phantom carries kPriorShotWeight, so two real shots already out-vote
// the prior. The ±10 magnitude on time_delta is intentionally larger than
// real-shot scatter — that pushes std_y up and dampens real-shot signal
// while data is sparse, keeping the suggestion conservative. Lowering it to
// ±3 was tried (2026-05-18) and made things noticeably worse: the smaller
// scale shrank std_y so β_T (the climate slope) had relatively *more*
// influence per-σ, and the solver compensated with absurd grind moves
// (suggested 1.7 instead of 4.4 in the hot-climate real-data test). The
// real fix for that scenario lives in the climate-extrapolation clamp
// (kClimateClampZ) below, not here.
struct PriorShot {
  float user_grind;
  float time_delta_s;
};
constexpr PriorShot kPriorShots[] = {
    {5.0f, +10.0f},  // finer  → longer
    {6.0f, -10.0f},  // coarser → shorter
};
constexpr float kPriorShotWeight = 0.5f;

// Typical room conditions phantom shots are pinned to. Real shots in most
// homes land within a few % of these; after standardization the phantom
// climate values come out near zero and contribute almost nothing to the
// climate slopes. Drifting these to actual house climate would let phantoms
// leak more into β_T/β_H/β_P, which is exactly what we don't want — the
// prior is for grind, not climate.
constexpr float kPriorShotTempC    =   22.0f;
constexpr float kPriorShotHumidity =   50.0f;
constexpr float kPriorShotPressure = 1013.0f;

// Don't try to surface a suggestion until at least this many usable shots
// exist for the preset. Below the floor the prior dominates entirely; the
// number we'd display would just be the prior mean (~no useful guidance) at
// near-0 confidence. Hard-cutting is cheaper than computing it.
constexpr std::size_t kMinShotsForFit = 2;

// Standard-deviation floor when scaling features. Two purposes: (1) avoids
// divide-by-zero when every shot used the same grind / climate, (2) suppresses
// over-amplification of features that vary by epsilon (sensor noise) by
// treating them as effectively zero-variance — the corresponding coefficient
// then can't dominate the suggestion.
constexpr float kStdFloor = 1e-3f;

// Largest absolute confidence we'll ever display. Hard-capping at 95% leaves
// the user a visual reminder that this is a model, not an oracle — even a
// rock-solid fit can be wrong about the next shot's noise. Also keeps the
// "5%-step rounded" output from ever reading 100%, which would mislead.
constexpr uint8_t kConfidenceCap = 95;

// Below this rounded percentage we suppress the suggestion entirely (return 0)
// instead of showing a near-meaningless single-digit indicator.
constexpr uint8_t kConfidenceFloor = 10;

// Maximum standard-deviation magnitude we'll let a climate feature take when
// solving for the suggested grind. Live climate readings outside this band
// (i.e. |z| > kClimateClampZ) get pinned to the band's edge. The math here:
// linear regression extrapolates linearly, but the assumption that the same
// slope holds far outside training is unsupported — we'd rather under-react
// to extreme climate than confidently project a slope from a 4-point dataset
// into a region we've never sampled. Honest defensive measure, not a
// principled probabilistic adjustment. 2.0σ keeps reasonable seasonal swings
// fully respected (typical room T ranges across ~2σ over a year of home
// brewing) while capping wild excursions.
constexpr float kClimateClampZ = 2.0f;

// Extrapolation confidence band. Same shape on both axes: full credit
// within ±kExtrapPenaltyStart σ, linear decay to zero at ±kExtrapPenaltyEnd σ.
// Applied to two independent z-scores:
//   - climate z: max(|T_z_raw|, |H_z_raw|, |P_z_raw|) using the (phantom-
//     blended) training mean/std. Asks "is the live climate in a region
//     we've sampled?"
//   - grind z: (suggested_grind - mean_g_real) / std_g_real using
//     real-shots-only stats. Asks "is the recommendation inside the user's
//     observed dial range?"
// Both factors enter the reported confidence as an unweighted average with
// the slope-quality factor (derived from Σ[0,0]). Averaging means no single
// factor can hide a useful suggestion, but each one drags the displayed
// number down to honest territory. Uses the *unclamped* z-values so the
// confidence channel surfaces extrapolation distance honestly even though
// the point-estimate solver clamps climate for safety.
constexpr float kExtrapPenaltyStart = 1.0f;
constexpr float kExtrapPenaltyEnd   = 3.0f;

// Floor on the real-shot std_g used by the grind-extrapolation factor. If
// the user has only ever pulled at one or two nearly-identical dial settings
// std_g_real collapses to zero and any suggestion away from the centroid
// would crash the factor to 0. Flooring at 0.1 (= one slider step) means
// "treat one dial step as the smallest meaningful tolerance band." Effect:
// suggestions within ±1 step of where the user has been get full credit;
// ±3 steps out gets zero credit. Tight by design — the whole point of this
// factor is to surface "you're recommending something outside the user's
// observed range."
constexpr float kGrindStdFloor = 0.1f;

// In-place Gauss-Jordan inverse of an n×n matrix with partial pivoting. We
// only ever invert n=kNFeat (4×4) so stack-allocating the augmented matrix is
// fine and avoids dragging in std::vector / a heap allocation per refit. With
// the +λI ridge regularization the input is strictly diagonally dominant in
// expectation, so the partial pivot is more defensive than necessary — kept
// anyway because numerical drift on float32 is cheap to guard against.
//
// Returns false on singular (won't happen in practice given the ridge); the
// caller treats that as "fit failed, no suggestion".
bool invert(float* mat, int n) {
  constexpr int kMax = kNFeat;
  if (n > kMax) return false;
  float aug[kMax][2 * kMax] = {};
  for (int i = 0; i < n; ++i) {
    for (int j = 0; j < n; ++j) aug[i][j] = mat[i * n + j];
    aug[i][n + i] = 1.0f;
  }
  for (int col = 0; col < n; ++col) {
    int pivot = col;
    float maxv = std::fabs(aug[col][col]);
    for (int i = col + 1; i < n; ++i) {
      const float v = std::fabs(aug[i][col]);
      if (v > maxv) { maxv = v; pivot = i; }
    }
    if (maxv < 1e-9f) return false;
    if (pivot != col) {
      for (int j = 0; j < 2 * n; ++j) std::swap(aug[col][j], aug[pivot][j]);
    }
    const float p = aug[col][col];
    for (int j = 0; j < 2 * n; ++j) aug[col][j] /= p;
    for (int i = 0; i < n; ++i) {
      if (i == col) continue;
      const float f = aug[i][col];
      if (f == 0.0f) continue;
      for (int j = 0; j < 2 * n; ++j) aug[i][j] -= f * aug[col][j];
    }
  }
  for (int i = 0; i < n; ++i)
    for (int j = 0; j < n; ++j)
      mat[i * n + j] = aug[i][n + j];
  return true;
}

// Maps a 0..1 confidence factor to a rounded/floored/capped percentage. The
// hard cap at kConfidenceCap (95%) reminds the user this is a model, not an
// oracle — there's still irreducible shot-to-shot noise we don't surface in
// this number. Anything below kConfidenceFloor snaps to 0 so the UI shows
// the "learning..." placeholder rather than a near-meaningless single-digit.
uint8_t confidence_pct_from_factor(float factor) {
  factor = std::clamp(factor, 0.0f, 1.0f);
  const int pct = static_cast<int>(std::lround(factor * 100.0f / 5.0f) * 5);
  if (pct < kConfidenceFloor) return 0;
  if (pct > kConfidenceCap)   return kConfidenceCap;
  return static_cast<uint8_t>(pct);
}

}  // namespace

PresetFit fit(const FitSample* samples, std::size_t n_real) {
  PresetFit f = {};
  if (n_real < kMinShotsForFit) return f;

  // --- Step 1: weighted means + stds ------------------------------------
  // Standardizing per-feature lets the single λ act as the right ridge for
  // all coefficients regardless of their raw scale (pressure varies in
  // hundreds of hPa; grind in single digits). Real shots contribute at w=1;
  // phantoms at w=kPriorShotWeight.
  double w_sum = 0.0;
  double sum_g = 0, sum_T = 0, sum_H = 0, sum_P = 0, sum_y = 0;
  for (std::size_t i = 0; i < n_real; ++i) {
    w_sum += 1.0;
    sum_g += samples[i].user_grind;
    sum_T += samples[i].temp_c;
    sum_H += samples[i].humidity_pct;
    sum_P += samples[i].pressure_hpa;
    sum_y += samples[i].time_delta_s;
  }
  for (const auto& ph : kPriorShots) {
    w_sum += kPriorShotWeight;
    sum_g += kPriorShotWeight * ph.user_grind;
    sum_T += kPriorShotWeight * kPriorShotTempC;
    sum_H += kPriorShotWeight * kPriorShotHumidity;
    sum_P += kPriorShotWeight * kPriorShotPressure;
    sum_y += kPriorShotWeight * ph.time_delta_s;
  }

  f.mean_g = static_cast<float>(sum_g / w_sum);
  f.mean_T = static_cast<float>(sum_T / w_sum);
  f.mean_H = static_cast<float>(sum_H / w_sum);
  f.mean_P = static_cast<float>(sum_P / w_sum);
  f.mean_y = static_cast<float>(sum_y / w_sum);

  double var_g = 0, var_T = 0, var_H = 0, var_P = 0, var_y = 0;
  for (std::size_t i = 0; i < n_real; ++i) {
    const float dg = samples[i].user_grind   - f.mean_g;
    const float dT = samples[i].temp_c       - f.mean_T;
    const float dH = samples[i].humidity_pct - f.mean_H;
    const float dP = samples[i].pressure_hpa - f.mean_P;
    const float dy = samples[i].time_delta_s - f.mean_y;
    var_g += dg * dg;
    var_T += dT * dT;
    var_H += dH * dH;
    var_P += dP * dP;
    var_y += dy * dy;
  }
  for (const auto& ph : kPriorShots) {
    const float dg = ph.user_grind       - f.mean_g;
    const float dT = kPriorShotTempC     - f.mean_T;
    const float dH = kPriorShotHumidity  - f.mean_H;
    const float dP = kPriorShotPressure  - f.mean_P;
    const float dy = ph.time_delta_s     - f.mean_y;
    var_g += kPriorShotWeight * dg * dg;
    var_T += kPriorShotWeight * dT * dT;
    var_H += kPriorShotWeight * dH * dH;
    var_P += kPriorShotWeight * dP * dP;
    var_y += kPriorShotWeight * dy * dy;
  }
  f.std_g = std::max(kStdFloor, static_cast<float>(std::sqrt(var_g / w_sum)));
  f.std_T = std::max(kStdFloor, static_cast<float>(std::sqrt(var_T / w_sum)));
  f.std_H = std::max(kStdFloor, static_cast<float>(std::sqrt(var_H / w_sum)));
  f.std_P = std::max(kStdFloor, static_cast<float>(std::sqrt(var_P / w_sum)));
  f.std_y = std::max(kStdFloor, static_cast<float>(std::sqrt(var_y / w_sum)));

  // Real-shots-only grind centroid and spread, for the grind-extrapolation
  // confidence factor. Separate from f.mean_g / f.std_g (which fold phantoms
  // in for the fit math) so the factor reflects user behavior, not the
  // prior. Floored at kGrindStdFloor so single-dial-setting histories don't
  // make the factor pathological.
  double sum_g_r = 0.0;
  for (std::size_t i = 0; i < n_real; ++i) sum_g_r += samples[i].user_grind;
  f.mean_g_real = static_cast<float>(sum_g_r / n_real);
  double var_g_r = 0.0;
  for (std::size_t i = 0; i < n_real; ++i) {
    const float d = samples[i].user_grind - f.mean_g_real;
    var_g_r += d * d;
  }
  f.std_g_real = std::max(kGrindStdFloor,
                          static_cast<float>(std::sqrt(var_g_r / n_real)));

  // --- Step 2: form XᵀWX (kNFeat×kNFeat) and XᵀWy in standardized space.
  // For weighted Bayesian regression with zero-mean Gaussian prior:
  //   posterior precision Λ = XᵀWX + λI
  //   posterior mean β     = Λ⁻¹ XᵀWy
  // We build XᵀWX directly by accumulating outer products row-by-row so we
  // never allocate the full X matrix.
  float A[kNFeat * kNFeat] = {};
  float b[kNFeat]          = {};
  auto accumulate = [&](float w, float g_raw, float T_raw, float H_raw,
                        float P_raw, float y_raw) {
    const float x[kNFeat] = {
        (g_raw - f.mean_g) / f.std_g,
        (T_raw - f.mean_T) / f.std_T,
        (H_raw - f.mean_H) / f.std_H,
        (P_raw - f.mean_P) / f.std_P,
    };
    const float y = (y_raw - f.mean_y) / f.std_y;
    for (int r = 0; r < kNFeat; ++r) {
      b[r] += w * x[r] * y;
      for (int c = 0; c < kNFeat; ++c) {
        A[r * kNFeat + c] += w * x[r] * x[c];
      }
    }
  };
  for (std::size_t i = 0; i < n_real; ++i) {
    accumulate(1.0f, samples[i].user_grind, samples[i].temp_c,
               samples[i].humidity_pct, samples[i].pressure_hpa,
               samples[i].time_delta_s);
  }
  for (const auto& ph : kPriorShots) {
    accumulate(kPriorShotWeight, ph.user_grind, kPriorShotTempC,
               kPriorShotHumidity, kPriorShotPressure, ph.time_delta_s);
  }
  for (int r = 0; r < kNFeat; ++r) A[r * kNFeat + r] += kPriorPrec;

  // --- Step 3: invert A → Σ (posterior covariance scaled by σ²=1) -------
  std::memcpy(f.sigma, A, sizeof(f.sigma));
  if (!invert(f.sigma, kNFeat)) return f;  // .valid stays false

  // β = Σ b
  for (int r = 0; r < kNFeat; ++r) {
    float acc = 0.0f;
    for (int c = 0; c < kNFeat; ++c) acc += f.sigma[r * kNFeat + c] * b[c];
    f.beta[r] = acc;
  }

  f.valid  = true;
  f.n_used = static_cast<uint16_t>(n_real);  // real shots only; phantoms always present
  return f;
}

Suggestion suggest(const PresetFit& f, ClimateInput c, float target_time_delta) {
  Suggestion none = {std::nanf(""), 0};
  if (!f.valid) return none;

  // grind_z is unknown — we're solving for it. Plug the target time_delta
  // into the standardized regression equation:
  //   y_z = β_g·g_z + β_T·T_z + β_H·H_z + β_P·P_z
  //   target y_raw = target_time_delta → target y_z = (target - mean_y) / std_y
  // Rearrange:
  //   g_z = (target_y_z - β_T·T_z - β_H·H_z - β_P·P_z) / β_g
  // Standardize climate to raw z-values first — the confidence channel uses
  // them un-clamped so it can surface "you're extrapolating" honestly. Then
  // produce clamped versions for the point-estimate solver, which we want
  // bounded against runaway extrapolation regardless of confidence.
  const float T_z_raw = (c.temp_c       - f.mean_T) / f.std_T;
  const float H_z_raw = (c.humidity_pct - f.mean_H) / f.std_H;
  const float P_z_raw = (c.pressure_hpa - f.mean_P) / f.std_P;
  const float T_z = std::clamp(T_z_raw, -kClimateClampZ, kClimateClampZ);
  const float H_z = std::clamp(H_z_raw, -kClimateClampZ, kClimateClampZ);
  const float P_z = std::clamp(P_z_raw, -kClimateClampZ, kClimateClampZ);
  const float target_y_z = (target_time_delta - f.mean_y) / f.std_y;

  // β_g (grind coefficient) near zero means the model didn't learn a useful
  // grind→time relationship — usually because every shot used the same grind
  // AND the phantoms got swamped. Dividing by it would produce a wild number
  // with no support. Suppress.
  if (std::fabs(f.beta[0]) < 1e-3f) return none;

  const float g_z = (target_y_z - f.beta[1] * T_z
                                - f.beta[2] * H_z
                                - f.beta[3] * P_z) / f.beta[0];

  // De-standardize back to the grinder's dial units.
  float grind = g_z * f.std_g + f.mean_g;
  grind = std::clamp(grind, 0.0f, 99.9f);
  grind = std::round(grind * 10.0f) / 10.0f;  // 0.1 step — matches UI stepper

  // Confidence is the average of three 0..1 factors:
  //   (1) slope_factor:        how well we know β_g, from Σ[0,0]. At zero
  //                            data it's 0 (prior-only); climbs as real
  //                            shots pin down the slope.
  //   (2) climate_extrap_factor: how close the live climate is to training.
  //                              Full credit within kExtrapPenaltyStart σ,
  //                              linear decay to zero by kExtrapPenaltyEnd σ.
  //                              Uses raw (unclamped) z so the honesty isn't
  //                              hidden by the point-estimate clamp.
  //   (3) grind_extrap_factor:   how close the *recommended grind* is to the
  //                              dial settings the user has actually pulled.
  //                              Same shape as (2), but using the
  //                              real-shots-only mean_g_real/std_g_real (no
  //                              phantoms — so a model that's never seen
  //                              the user move the dial gets honestly
  //                              penalized when it recommends moving it).
  // Averaging means no single factor can hide a useful suggestion, but each
  // one drags the displayed number toward honest territory when warranted.
  const float slope_factor = std::clamp(1.0f - f.sigma[0] * kPriorPrec, 0.0f, 1.0f);
  const float z_climate = std::max({std::fabs(T_z_raw),
                                    std::fabs(H_z_raw),
                                    std::fabs(P_z_raw)});
  const float climate_extrap_factor = std::clamp(
      1.0f - std::max(0.0f, z_climate - kExtrapPenaltyStart) /
                 (kExtrapPenaltyEnd - kExtrapPenaltyStart),
      0.0f, 1.0f);
  const float z_grind = std::fabs((grind - f.mean_g_real) / f.std_g_real);
  const float grind_extrap_factor = std::clamp(
      1.0f - std::max(0.0f, z_grind - kExtrapPenaltyStart) /
                 (kExtrapPenaltyEnd - kExtrapPenaltyStart),
      0.0f, 1.0f);
  const uint8_t conf = confidence_pct_from_factor(
      (slope_factor + climate_extrap_factor + grind_extrap_factor) / 3.0f);
  if (conf == 0) return none;

  return Suggestion{grind, conf};
}

}  // namespace espressopost::model
