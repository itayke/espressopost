#include "model_math.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace espressopost::model {
namespace {

// Time model features after centering/scaling: grind, temp, humidity, pressure
// (kNCont, in the header), optionally followed by per-epoch intercept dummies.
// (Intercept absorbed by centering y and X.) Spec calls for two models (time
// + quality); v1 ships time-only because it has the cleanest grind→target
// relationship. Quality model + cross-preset pooling are follow-ups, gated
// behind enough data to make extra parameters earn their keep.

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
// pull (assumed convention; flip the phantom y signs if a future grinder's
// dial runs the other way). Climate values land at typical room conditions
// so the phantoms barely touch β_T/β_H/β_P after standardization.
//
// The model regresses LOG brew time (see fit()), so the phantom's time anchor
// is expressed as a symmetric log-ratio around the real-shot log-mean: the
// finer phantom sits at log_mean + ln(kPriorShotTimeRatio), the coarser at
// log_mean − ln(kPriorShotTimeRatio). A multiplicative offset reads the same
// whether the user pulls 22 s or 40 s shots, and stays symmetric in the space
// the regression actually lives in (a fixed ±seconds offset would not).
//
// Two purposes:
//   1) Inject grind variance — your first dozen shots may all be at the same
//      dial setting, leaving std_g floored and β_g unlearnable. Phantoms fix
//      that on day one.
//   2) Bias β_g to the right sign with non-trivial magnitude, so the very
//      first suggestion is at least pointing the user the right way.
//
// Phantom grind positions are built at fit time, symmetric around the real-
// shot grind mean (mean ± kPriorShotGrindSpread). That way the prior adapts
// to wherever the user actually grinds — any 1-50 dial, any operating
// point — instead of pinning to the historical 1-10 midpoint we picked when
// scaffolding. Symmetric placement also makes the phantom contribution to
// mean_g algebraically cancel: f.mean_g ends up equal to the real-shot
// grind mean, so suggest() can use it directly as the "user's observed
// dial centroid" without a separate field.
//
// Each phantom carries kPriorShotWeight, so two real shots already out-vote
// the prior. kPriorShotTimeRatio is intentionally larger than real-shot
// log-scatter — it pushes std_y up and dampens real-shot signal while data is
// sparse, keeping the suggestion conservative. The seconds-space predecessor
// (±10 s) was tuned the same way; shrinking it (±3 s tried 2026-05-18) made
// β_T relatively more influential per-σ and the solver compensated with absurd
// grind moves. The real fix for far-climate extrapolation lives in the climate
// clamp (kClimateClampZ) below, not here.
//
// Spread (±0.5) is a soft knob — after standardization, the algebra cancels
// most of its effect on β_g. Picked to read as "small dial movement" to a
// human; the model behaves nearly identically at ±0.1 or ±5.0.
struct PriorShot {
  float user_grind;
  float log_delta_from_y_center;  // signed LOG-seconds offset from the real-shot log-mean brew time (= ±ln(kPriorShotTimeRatio))
};
constexpr float kPriorShotWeight     = 0.5f;
constexpr float kPriorShotGrindSpread = 0.5f;

// Phantom brew-time spread as a multiplicative ratio in log space: the finer
// phantom is this factor slower than the real-shot geometric-mean time, the
// coarser one this factor faster. ~1.4 ≈ the old ±10 s anchor at a ~28 s mean,
// kept deliberately wide so the prior stays conservative while data is sparse.
constexpr float kPriorShotTimeRatio = 1.4f;

// Floor on brew seconds before taking the log, so a stray 0 s record (or any
// sub-second value) can't blow log() to -inf. Real espresso shots are tens of
// seconds, so this only ever guards corrupt/empty inputs.
constexpr float kMinBrewSecondsForLog = 1.0f;

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
//   - grind z: (suggested_grind - mean_g) / std_g_real. mean_g coincides
//     with the real-shot grind centroid by construction (symmetric phantom
//     placement); std_g_real excludes phantoms so this asks "is the
//     recommendation inside the user's observed dial range?"
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

// Out-of-band tip gates (classify_shot). The tip only fires when the model was
// confident enough to have a defensible opinion (>= gate) AND the actual brew
// time missed the prediction by more than this multiplicative margin. The band
// is symmetric in log space: kTipBandRatio=1.4 flags a shot >40% slower OR
// (1/1.4 ≈) >29% faster than predicted. Conservative on purpose — a tip that
// fires on ordinary shot-to-shot noise trains the user to ignore it.
constexpr uint8_t kTipConfidenceGate = 80;
constexpr float   kTipBandRatio      = 1.4f;

// Brew seconds → model y-space (log seconds), with the corrupt-input floor.
// Single source of truth for the seconds⇄log boundary so fit(), suggest(),
// predict_time_s() and classify_shot() all transform identically.
inline float log_time_s(float seconds) {
  return std::log(std::max(seconds, kMinBrewSecondsForLog));
}

// In-place Gauss-Jordan inverse of an n×n matrix with partial pivoting. We
// only ever invert n ≤ kMaxFeat so stack-allocating the augmented matrix is
// fine and avoids dragging in std::vector / a heap allocation per refit. With
// the +λI ridge regularization the input is strictly diagonally dominant in
// expectation, so the partial pivot is more defensive than necessary — kept
// anyway because numerical drift on float32 is cheap to guard against.
//
// Returns false on singular (won't happen in practice given the ridge); the
// caller treats that as "fit failed, no suggestion".
bool invert(float* mat, int n) {
  constexpr int kMax = kMaxFeat;
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

PresetFit fit(const FitSample* samples, std::size_t n_real, uint8_t n_epochs) {
  PresetFit f = {};
  if (n_real < kMinShotsForFit) return f;

  // --- Step 0a: epoch-dummy layout -------------------------------------
  // Map calibration epochs onto regression columns. The latest epoch is the
  // reference (no column — suggest()/predict() evaluate there); each OLDER
  // epoch that actually has shots earns one centered-0/1 dummy column appended
  // after the four continuous features. Empty older epochs get no column (the
  // ridge would just zero them anyway). If the reference epoch itself has no
  // shots we can't anchor "now," so we fall back to a single pooled fit — the
  // same path n_epochs ≤ 1 takes, byte-identical to the pre-epoch model.
  if (n_epochs < 1) n_epochs = 1;
  if (n_epochs > kMaxEpochs) n_epochs = kMaxEpochs;
  const int ref_epoch = n_epochs - 1;

  uint16_t epoch_n[kMaxEpochs] = {};
  auto epoch_of = [&](const FitSample& s) -> int {
    int e = s.epoch_index;
    if (e < 0) e = 0;
    if (e > ref_epoch) e = ref_epoch;
    return e;
  };
  for (std::size_t i = 0; i < n_real; ++i) ++epoch_n[epoch_of(samples[i])];

  int dummy_col[kMaxEpochs];
  for (int e = 0; e < kMaxEpochs; ++e) dummy_col[e] = -1;
  int n_dummies = 0;
  if (n_epochs >= 2 && epoch_n[ref_epoch] > 0) {
    for (int e = 0; e < ref_epoch; ++e)
      if (epoch_n[e] > 0) dummy_col[e] = kNCont + (n_dummies++);
  }
  const int n_dim = kNCont + n_dummies;

  // --- Step 0: real-shot centroids drive phantom placement -------------
  // Phantoms are built symmetrically around the user's actual operating
  // point so the directional prior ("finer→longer") lands near real data
  // regardless of where on the dial the user lives. The symmetric ±spread
  // placement also makes the phantom contributions to mean_g cancel out,
  // so f.mean_g (computed below across all weighted samples) will equal
  // this real-only centroid algebraically.
  //
  // The regression works in LOG brew time, so the phantom time anchor follows
  // the real-shot log-mean: finer phantom = log_mean + ln(ratio), coarser =
  // log_mean − ln(ratio). A multiplicative (log) offset keeps the prior shape
  // sensible whether the user pulls 22 s shots or 40 s shots, and symmetric in
  // the space the fit lives in.
  double sum_g_real = 0.0;
  double sum_y_real = 0.0;  // accumulates LOG brew time
  for (std::size_t i = 0; i < n_real; ++i) {
    sum_g_real += samples[i].user_grind;
    sum_y_real += log_time_s(samples[i].actual_time_s);
  }
  const float g_real_mean    = static_cast<float>(sum_g_real / n_real);
  const float y_real_logmean = static_cast<float>(sum_y_real / n_real);
  const float log_delta      = std::log(kPriorShotTimeRatio);
  const PriorShot phantoms[] = {
      {g_real_mean - kPriorShotGrindSpread, +log_delta},  // finer  → longer
      {g_real_mean + kPriorShotGrindSpread, -log_delta},  // coarser → shorter
  };
  auto phantom_y = [&](const PriorShot& ph) {
    return y_real_logmean + ph.log_delta_from_y_center;
  };

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
    sum_y += log_time_s(samples[i].actual_time_s);
  }
  for (const auto& ph : phantoms) {
    w_sum += kPriorShotWeight;
    sum_g += kPriorShotWeight * ph.user_grind;
    sum_T += kPriorShotWeight * kPriorShotTempC;
    sum_H += kPriorShotWeight * kPriorShotHumidity;
    sum_P += kPriorShotWeight * kPriorShotPressure;
    sum_y += kPriorShotWeight * phantom_y(ph);
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
    const float dy = log_time_s(samples[i].actual_time_s) - f.mean_y;
    var_g += dg * dg;
    var_T += dT * dT;
    var_H += dH * dH;
    var_P += dP * dP;
    var_y += dy * dy;
  }
  for (const auto& ph : phantoms) {
    const float dg = ph.user_grind       - f.mean_g;
    const float dT = kPriorShotTempC     - f.mean_T;
    const float dH = kPriorShotHumidity  - f.mean_H;
    const float dP = kPriorShotPressure  - f.mean_P;
    const float dy = phantom_y(ph)        - f.mean_y;
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

  // Real-shots-only grind spread, for the grind-extrapolation confidence
  // factor. Floored at kGrindStdFloor so single-dial-setting histories
  // don't make the factor pathological. (No matching mean_g_real because
  // symmetric phantom placement makes f.mean_g == this real-shot mean
  // algebraically — see Step 0.)
  double var_g_r = 0.0;
  for (std::size_t i = 0; i < n_real; ++i) {
    const float d = samples[i].user_grind - g_real_mean;
    var_g_r += d * d;
  }
  f.std_g_real = std::max(kGrindStdFloor,
                          static_cast<float>(std::sqrt(var_g_r / n_real)));

  // --- Step 1b: epoch-dummy means (deviation coding) -------------------
  // Each older-epoch column is a raw 0/1 indicator; we CENTER it (subtract its
  // weighted mean) but deliberately do NOT scale it to unit variance. With one
  // shared ridge λ, an unscaled centered dummy self-regularizes by population:
  // its XᵀWX diagonal is w_sum·m(1−m), so a sparse epoch (small m) is shrunk
  // hard toward the reference while a well-populated one is trusted — exactly
  // the "ridge shrinks sparse epochs toward 0" behavior we want, and stable
  // (no divide-by-tiny-std blow-up a rare epoch would cause if standardized).
  // Phantoms sit at the reference epoch (raw dummy 0), so they only contribute
  // to the denominator. Means index by dummy slot j (column kNCont + j).
  float dummy_mean[kMaxFeat - kNCont] = {};
  if (n_dummies > 0) {
    double dsum[kMaxFeat - kNCont] = {};
    for (std::size_t i = 0; i < n_real; ++i) {
      const int col = dummy_col[epoch_of(samples[i])];
      if (col >= 0) dsum[col - kNCont] += 1.0;  // weight 1; phantoms add 0
    }
    for (int j = 0; j < n_dummies; ++j)
      dummy_mean[j] = static_cast<float>(dsum[j] / w_sum);
  }

  // --- Step 2: form XᵀWX (n_dim×n_dim) and XᵀWy in standardized space.
  // For weighted Bayesian regression with zero-mean Gaussian prior:
  //   posterior precision Λ = XᵀWX + λI
  //   posterior mean β     = Λ⁻¹ XᵀWy
  // We build XᵀWX directly by accumulating outer products row-by-row so we
  // never allocate the full X matrix. The first kNCont columns are the
  // standardized continuous features; any trailing columns are the centered
  // epoch dummies (`dcol` is the column this sample's epoch lights up, or −1
  // for a reference-epoch shot / phantom).
  float A[kMaxFeat * kMaxFeat] = {};
  float b[kMaxFeat]            = {};
  auto accumulate = [&](float w, int dcol, float g_raw, float T_raw,
                        float H_raw, float P_raw, float y_raw) {
    float x[kMaxFeat];
    x[0] = (g_raw - f.mean_g) / f.std_g;
    x[1] = (T_raw - f.mean_T) / f.std_T;
    x[2] = (H_raw - f.mean_H) / f.std_H;
    x[3] = (P_raw - f.mean_P) / f.std_P;
    for (int j = 0; j < n_dummies; ++j) {
      const float raw = (dcol == kNCont + j) ? 1.0f : 0.0f;
      x[kNCont + j] = raw - dummy_mean[j];  // deviation coding, unscaled
    }
    const float y = (y_raw - f.mean_y) / f.std_y;
    for (int r = 0; r < n_dim; ++r) {
      b[r] += w * x[r] * y;
      for (int c = 0; c < n_dim; ++c) A[r * n_dim + c] += w * x[r] * x[c];
    }
  };
  for (std::size_t i = 0; i < n_real; ++i) {
    accumulate(1.0f, dummy_col[epoch_of(samples[i])], samples[i].user_grind,
               samples[i].temp_c, samples[i].humidity_pct,
               samples[i].pressure_hpa, log_time_s(samples[i].actual_time_s));
  }
  for (const auto& ph : phantoms) {
    accumulate(kPriorShotWeight, /*dcol=*/-1, ph.user_grind, kPriorShotTempC,
               kPriorShotHumidity, kPriorShotPressure, phantom_y(ph));
  }
  for (int r = 0; r < n_dim; ++r) A[r * n_dim + r] += kPriorPrec;

  // --- Step 3: invert A → Σ (posterior covariance scaled by σ²=1) -------
  // f.sigma is stored at stride n_dim (the leading n_dim×n_dim block); the
  // unused tail stays zero from the f = {} init.
  std::memcpy(f.sigma, A, sizeof(float) * n_dim * n_dim);
  if (!invert(f.sigma, n_dim)) return f;  // .valid stays false

  // β = Σ b
  for (int r = 0; r < n_dim; ++r) {
    float acc = 0.0f;
    for (int c = 0; c < n_dim; ++c) acc += f.sigma[r * n_dim + c] * b[c];
    f.beta[r] = acc;
  }

  // --- Step 4: epoch reference offset + per-epoch readback -------------
  f.n_dim    = static_cast<uint8_t>(n_dim);
  f.n_epochs = n_epochs;
  // Reference-epoch dummy values are (0 − meanⱼ), so the reference's
  // contribution to predicted y_z is −Σ βⱼ·meanⱼ. suggest()/predict() fold
  // this in so they evaluate "as the setup behaves now" even though y was
  // centered across all epochs. Zero when pooled (no dummy columns).
  float y_z_ref = 0.0f;
  for (int j = 0; j < n_dummies; ++j)
    y_z_ref += f.beta[kNCont + j] * (0.0f - dummy_mean[j]);
  f.y_z_ref_dummy = y_z_ref;

  // Per-epoch dial offset vs the reference, in grind units. A centered dummy's
  // coefficient is exactly that epoch's y_z offset from the reference (the 0→1
  // jump equals βⱼ); convert to grind via Δg = δ·std_g/β_g. Reference = 0;
  // epochs with no column (empty, or a pooled fit) report NaN so the Events
  // readback shows "gathering data" instead of a fake zero.
  const bool have_slope = std::fabs(f.beta[0]) > 1e-3f;
  for (int e = 0; e < kMaxEpochs; ++e) {
    f.epoch_n_used[e]       = (e < n_epochs) ? epoch_n[e] : 0;
    f.epoch_grind_offset[e] = std::nanf("");
  }
  f.epoch_grind_offset[ref_epoch] = 0.0f;  // latest epoch is the reference
  for (int e = 0; e < ref_epoch; ++e) {
    const int col = dummy_col[e];
    if (col >= 0 && have_slope)
      f.epoch_grind_offset[e] = f.beta[col] * f.std_g / f.beta[0];
  }

  f.valid  = true;
  f.n_used = static_cast<uint16_t>(n_real);  // real shots only; phantoms always present
  return f;
}

Suggestion suggest(const PresetFit& f, ClimateInput c, float target_time_s) {
  Suggestion none = {std::nanf(""), 0};
  if (!f.valid) return none;

  // grind_z is unknown — we're solving for it. Plug the target brew time
  // into the standardized regression equation:
  //   y_z = β_g·g_z + β_T·T_z + β_H·H_z + β_P·P_z
  //   target y_z = (log(target_time_s) - mean_y) / std_y   (y is log-seconds)
  // Rearrange:
  //   g_z = (target_y_z - β_T·T_z - β_H·H_z - β_P·P_z) / β_g
  // Because records now carry absolute brew seconds, retuning a preset's
  // target only shifts target_y_z — β stays put. Standardize climate to raw
  // z-values first — the confidence channel uses them un-clamped so it can
  // surface "you're extrapolating" honestly. Then produce clamped versions
  // for the point-estimate solver, which we want bounded against runaway
  // extrapolation regardless of confidence.
  const float T_z_raw = (c.temp_c       - f.mean_T) / f.std_T;
  const float H_z_raw = (c.humidity_pct - f.mean_H) / f.std_H;
  const float P_z_raw = (c.pressure_hpa - f.mean_P) / f.std_P;
  const float T_z = std::clamp(T_z_raw, -kClimateClampZ, kClimateClampZ);
  const float H_z = std::clamp(H_z_raw, -kClimateClampZ, kClimateClampZ);
  const float P_z = std::clamp(P_z_raw, -kClimateClampZ, kClimateClampZ);
  const float target_y_z = (log_time_s(target_time_s) - f.mean_y) / f.std_y;

  // β_g (grind coefficient) near zero means the model didn't learn a useful
  // grind→time relationship — usually because every shot used the same grind
  // AND the phantoms got swamped. Dividing by it would produce a wild number
  // with no support. Suppress.
  if (std::fabs(f.beta[0]) < 1e-3f) return none;

  // Solve at the reference (latest) epoch: y_z_ref_dummy is the constant the
  // epoch-offset model adds for "now" (0 when pooled), so it moves to the
  // numerator alongside the climate terms.
  const float g_z = (target_y_z - f.beta[1] * T_z
                                - f.beta[2] * H_z
                                - f.beta[3] * P_z
                                - f.y_z_ref_dummy) / f.beta[0];

  // De-standardize back to the grinder's dial units.
  float grind = g_z * f.std_g + f.mean_g;
  grind = std::clamp(grind, 0.0f, 99.9f);
  grind = std::round(grind / kGrindStep) * kGrindStep;  // snap to dial step

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
  //                              Same shape as (2); uses f.mean_g (which
  //                              equals the real-shot centroid by symmetric
  //                              phantom placement) and f.std_g_real (real
  //                              shots only). A model that's never seen the
  //                              user move the dial gets honestly penalized
  //                              when it recommends moving it.
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
  const float z_grind = std::fabs((grind - f.mean_g) / f.std_g_real);
  const float grind_extrap_factor = std::clamp(
      1.0f - std::max(0.0f, z_grind - kExtrapPenaltyStart) /
                 (kExtrapPenaltyEnd - kExtrapPenaltyStart),
      0.0f, 1.0f);
  const uint8_t conf = confidence_pct_from_factor(
      (slope_factor + climate_extrap_factor + grind_extrap_factor) / 3.0f);
  if (conf == 0) return none;

  return Suggestion{grind, conf};
}

float predict_time_s(const PresetFit& f, ClimateInput c, float grind) {
  if (!f.valid) return std::nanf("");

  // Standardize inputs into the space β lives in. Climate is clamped exactly as
  // suggest() clamps it (kClimateClampZ) so a wild live reading can't project
  // the slope into a region we never sampled. Grind is the shot's own dial
  // setting, used as-is — we want the honest prediction for what was pulled.
  const float g_z = (grind - f.mean_g) / f.std_g;
  const float T_z = std::clamp((c.temp_c       - f.mean_T) / f.std_T,
                               -kClimateClampZ, kClimateClampZ);
  const float H_z = std::clamp((c.humidity_pct - f.mean_H) / f.std_H,
                               -kClimateClampZ, kClimateClampZ);
  const float P_z = std::clamp((c.pressure_hpa - f.mean_P) / f.std_P,
                               -kClimateClampZ, kClimateClampZ);

  // Evaluate at the reference (latest) epoch — y_z_ref_dummy carries the
  // epoch-offset constant for "now" (0 when pooled), matching suggest().
  const float y_z = f.beta[0] * g_z + f.beta[1] * T_z
                  + f.beta[2] * H_z + f.beta[3] * P_z + f.y_z_ref_dummy;
  return std::exp(y_z * f.std_y + f.mean_y);  // log-seconds → seconds
}

ShotVerdict classify_shot(const PresetFit& f, ClimateInput c, float grind,
                          float actual_time_s, uint8_t confidence_pct) {
  // Stay silent unless the model had a confident opinion to defend. Below the
  // gate the prediction isn't trustworthy enough to call a shot "out of band,"
  // and a tip would just be noise on data the model can't model yet.
  if (!f.valid || confidence_pct < kTipConfidenceGate) return ShotVerdict::InBand;

  const float predicted = predict_time_s(f, c, grind);
  if (!std::isfinite(predicted) || predicted <= 0.0f) return ShotVerdict::InBand;

  // Residual in log space → a symmetric multiplicative band. |resid| within
  // ln(ratio) is the tolerated factor either way; outside it the sign gives the
  // direction (positive = actual slower than predicted = ran long).
  const float resid = log_time_s(actual_time_s) - std::log(predicted);
  if (std::fabs(resid) <= std::log(kTipBandRatio)) return ShotVerdict::InBand;
  return resid > 0.0f ? ShotVerdict::RanLong : ShotVerdict::RanShort;
}

}  // namespace espressopost::model
