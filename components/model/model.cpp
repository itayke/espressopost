#include "model.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "climate.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "presets.hpp"
#include "storage.hpp"

namespace espressopost::model {
namespace {

constexpr const char* kTag = "model";

// Time model features after centering/scaling: grind, temp, humidity, pressure.
// (Intercept is absorbed by centering y and X, see fit_preset below.) Spec
// calls for two models (time + quality); v1 ships the time model only — it
// has the cleanest grind→target relationship and lets us reach a useful UI
// suggestion with minimal code. Quality model + cross-preset pooling are
// follow-ups, gated behind enough data to make their extra parameters earn
// their keep.
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
// while data is sparse, keeping the suggestion conservative. Once ~20 real
// shots accumulate, real std_y dominates and phantoms fade to noise. If we
// observe the model staying glued to the prior too long, dial the magnitude
// (or the weight) down.
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
constexpr size_t kMinShotsForFit = 2;

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

// Hard cap on shots we load into RAM in one go. 40 B/record × 1024 = 40 KB
// fits comfortably and covers years of typical home use. Going past this would
// only matter for power users running many shots per day — at which point a
// paginated/streaming fit is warranted anyway.
constexpr size_t kMaxShotsLoaded = 1024;

struct PresetFit {
  bool   valid;
  uint16_t n_used;          // number of shots that contributed (after filters)

  // Per-feature standardization parameters captured at fit time. We need them
  // again at predict time to map the live climate into the same standardized
  // space the coefficients live in.
  float mean_g, std_g;
  float mean_T, std_T;
  float mean_H, std_H;
  float mean_P, std_P;
  float mean_y, std_y;

  // Standardized coefficients (β) and posterior precision-matrix inverse
  // (Σ) — both in standardized feature space. β is what we plug into the
  // suggestion solve; Σ is what we sandwich x*ᵀ Σ x* through for confidence.
  float beta[kNFeat];
  float sigma[kNFeat * kNFeat];
};

PresetFit         s_fits[presets::kMaxPresets] = {};
SemaphoreHandle_t s_lock = nullptr;

struct Guard {
  bool ok;
  explicit Guard(TickType_t timeout = portMAX_DELAY)
      : ok(xSemaphoreTake(s_lock, timeout) == pdTRUE) {}
  ~Guard() { if (ok) xSemaphoreGive(s_lock); }
};

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

// Returns true if this shot should be excluded from training. Tombstoned and
// anomaly-flagged records stay on disk for audit but must not influence the
// fit. Shots with no climate reading (timestamp_us==0 means the climate task
// hadn't sampled yet at submit time) are also excluded — three of our four
// features would otherwise read as 0, which both pollutes the fit and skews
// standardization.
bool excluded(const storage::ShotRecord& r) {
  if (r.flags & (storage::kFlagTombstone | storage::kFlagAnomaly)) return true;
  // If both climate and humidity are exactly zero, we never got a real reading
  // for this shot. (Real BME280 output is never exactly 0 for either.)
  if (r.temp_c == 0.0f && r.humidity_pct == 0.0f) return true;
  return false;
}

// Fit one preset from `shots` (already filtered to this preset_id). Writes
// into `s_fits[preset_id]`. Marks valid=false if too few real shots / a
// singular precision matrix (the latter shouldn't happen with the +λI ridge).
//
// The synthetic kPriorShots[] phantoms are folded into every pass below at
// weight kPriorShotWeight. That serves three purposes: (a) injects grind
// variance so std_g never sits at the floor while real shots cluster at the
// same setting, (b) seeds β_g with the right sign so the very first
// suggestion points the user the right way, (c) gracefully fades as real
// data accumulates since the phantoms' combined weight is fixed.
void fit_preset(uint8_t preset_id,
                const storage::ShotRecord* shots,
                size_t n) {
  PresetFit& fit = s_fits[preset_id];
  fit = {};

  if (n < kMinShotsForFit) {
    ESP_LOGD(kTag, "preset %u: only %u real shots (<%u floor) — skipping fit",
             preset_id, static_cast<unsigned>(n),
             static_cast<unsigned>(kMinShotsForFit));
    return;
  }

  // --- Step 1: weighted means + stds ------------------------------------
  // Standardizing per-feature lets the single λ act as the right ridge for
  // all coefficients regardless of their raw scale (pressure varies in
  // hundreds of hPa; grind in single digits). Real shots contribute at w=1;
  // phantoms at w=kPriorShotWeight.
  double w_sum = 0.0;
  double sum_g = 0, sum_T = 0, sum_H = 0, sum_P = 0, sum_y = 0;
  for (size_t i = 0; i < n; ++i) {
    w_sum += 1.0;
    sum_g += shots[i].user_grind;
    sum_T += shots[i].temp_c;
    sum_H += shots[i].humidity_pct;
    sum_P += shots[i].pressure_hpa;
    sum_y += shots[i].time_delta_s;
  }
  for (const auto& ph : kPriorShots) {
    w_sum += kPriorShotWeight;
    sum_g += kPriorShotWeight * ph.user_grind;
    sum_T += kPriorShotWeight * kPriorShotTempC;
    sum_H += kPriorShotWeight * kPriorShotHumidity;
    sum_P += kPriorShotWeight * kPriorShotPressure;
    sum_y += kPriorShotWeight * ph.time_delta_s;
  }

  fit.mean_g = static_cast<float>(sum_g / w_sum);
  fit.mean_T = static_cast<float>(sum_T / w_sum);
  fit.mean_H = static_cast<float>(sum_H / w_sum);
  fit.mean_P = static_cast<float>(sum_P / w_sum);
  fit.mean_y = static_cast<float>(sum_y / w_sum);

  double var_g = 0, var_T = 0, var_H = 0, var_P = 0, var_y = 0;
  for (size_t i = 0; i < n; ++i) {
    const float dg = shots[i].user_grind   - fit.mean_g;
    const float dT = shots[i].temp_c       - fit.mean_T;
    const float dH = shots[i].humidity_pct - fit.mean_H;
    const float dP = shots[i].pressure_hpa - fit.mean_P;
    const float dy = shots[i].time_delta_s - fit.mean_y;
    var_g += dg * dg;
    var_T += dT * dT;
    var_H += dH * dH;
    var_P += dP * dP;
    var_y += dy * dy;
  }
  for (const auto& ph : kPriorShots) {
    const float dg = ph.user_grind       - fit.mean_g;
    const float dT = kPriorShotTempC     - fit.mean_T;
    const float dH = kPriorShotHumidity  - fit.mean_H;
    const float dP = kPriorShotPressure  - fit.mean_P;
    const float dy = ph.time_delta_s     - fit.mean_y;
    var_g += kPriorShotWeight * dg * dg;
    var_T += kPriorShotWeight * dT * dT;
    var_H += kPriorShotWeight * dH * dH;
    var_P += kPriorShotWeight * dP * dP;
    var_y += kPriorShotWeight * dy * dy;
  }
  fit.std_g = std::max(kStdFloor, static_cast<float>(std::sqrt(var_g / w_sum)));
  fit.std_T = std::max(kStdFloor, static_cast<float>(std::sqrt(var_T / w_sum)));
  fit.std_H = std::max(kStdFloor, static_cast<float>(std::sqrt(var_H / w_sum)));
  fit.std_P = std::max(kStdFloor, static_cast<float>(std::sqrt(var_P / w_sum)));
  fit.std_y = std::max(kStdFloor, static_cast<float>(std::sqrt(var_y / w_sum)));

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
        (g_raw - fit.mean_g) / fit.std_g,
        (T_raw - fit.mean_T) / fit.std_T,
        (H_raw - fit.mean_H) / fit.std_H,
        (P_raw - fit.mean_P) / fit.std_P,
    };
    const float y = (y_raw - fit.mean_y) / fit.std_y;
    for (int r = 0; r < kNFeat; ++r) {
      b[r] += w * x[r] * y;
      for (int c = 0; c < kNFeat; ++c) {
        A[r * kNFeat + c] += w * x[r] * x[c];
      }
    }
  };
  for (size_t i = 0; i < n; ++i) {
    accumulate(1.0f, shots[i].user_grind, shots[i].temp_c,
               shots[i].humidity_pct, shots[i].pressure_hpa,
               shots[i].time_delta_s);
  }
  for (const auto& ph : kPriorShots) {
    accumulate(kPriorShotWeight, ph.user_grind, kPriorShotTempC,
               kPriorShotHumidity, kPriorShotPressure, ph.time_delta_s);
  }
  for (int r = 0; r < kNFeat; ++r) A[r * kNFeat + r] += kPriorPrec;

  // --- Step 3: invert A → Σ (posterior covariance scaled by σ²=1) -------
  std::memcpy(fit.sigma, A, sizeof(fit.sigma));
  if (!invert(fit.sigma, kNFeat)) {
    ESP_LOGW(kTag, "preset %u: singular precision matrix — skipping fit", preset_id);
    return;
  }

  // β = Σ b
  for (int r = 0; r < kNFeat; ++r) {
    float acc = 0.0f;
    for (int c = 0; c < kNFeat; ++c) acc += fit.sigma[r * kNFeat + c] * b[c];
    fit.beta[r] = acc;
  }

  fit.valid  = true;
  fit.n_used = static_cast<uint16_t>(n);  // real shots only; phantoms always present
}

// Solve a triple-quadratic form xᵀ Σ x for the predictive parameter variance.
// We only need it at one point (the candidate grind) per call, so a generic
// quadratic-form helper is overkill — the inlined loop is clearer.
float quad_form(const float* sigma, const float* x) {
  float acc = 0.0f;
  for (int r = 0; r < kNFeat; ++r) {
    float row = 0.0f;
    for (int c = 0; c < kNFeat; ++c) row += sigma[r * kNFeat + c] * x[c];
    acc += x[r] * row;
  }
  return acc;
}

// Maps the posterior parameter variance at the suggested grind to a 0..100
// integer in 5-unit steps. The kPriorPrec value sets the natural scale: at
// zero data, V_param ≈ ‖x*‖² / λ; multiplying by λ normalizes that to ~1 so
// confidence reads ~0% when the prior is doing all the work. With more data,
// V_param drops toward zero and confidence rises.
//
// We hard-cap at kConfidenceCap (95%) so users don't read a 100% as "the
// model is certain" — there's still irreducible shot-to-shot noise we don't
// surface in this number. We snap anything below kConfidenceFloor to 0 so
// the UI suppresses the row entirely rather than show "5%".
uint8_t confidence_from_variance(float v_param) {
  const float scaled = std::clamp(1.0f - v_param * kPriorPrec, 0.0f, 1.0f);
  const int pct      = static_cast<int>(std::lround(scaled * 100.0f / 5.0f) * 5);
  if (pct < kConfidenceFloor) return 0;
  if (pct > kConfidenceCap)   return kConfidenceCap;
  return static_cast<uint8_t>(pct);
}

}  // namespace

esp_err_t init() {
  if (s_lock != nullptr) return ESP_ERR_INVALID_STATE;
  s_lock = xSemaphoreCreateMutex();
  if (s_lock == nullptr) return ESP_ERR_NO_MEM;
  refit();
  return ESP_OK;
}

void refit() {
  if (s_lock == nullptr) return;
  Guard g;
  if (!g.ok) return;

  // Pull all shots into RAM. 40 KB worst case (1024 records × 40 B) — well
  // under any sane PSRAM budget. Heap not stack because kMaxShotsLoaded is
  // bigger than typical task stacks.
  auto* shots = static_cast<storage::ShotRecord*>(
      std::malloc(sizeof(storage::ShotRecord) * kMaxShotsLoaded));
  if (shots == nullptr) {
    ESP_LOGE(kTag, "refit: malloc for %u shots failed",
             static_cast<unsigned>(kMaxShotsLoaded));
    return;
  }
  const size_t n_total = storage::read_shots(shots, kMaxShotsLoaded);

  // Bucket by preset — single pass, indices only. Avoids copying records and
  // keeps per-preset memory at O(n_total) ints worst case.
  uint16_t per_preset_count[presets::kMaxPresets] = {};
  for (size_t i = 0; i < n_total; ++i) {
    const auto& r = shots[i];
    if (r.preset_id >= presets::kMaxPresets) continue;
    if (excluded(r)) continue;
    ++per_preset_count[r.preset_id];
  }

  const uint8_t n_presets = presets::count();
  size_t total_used = 0;
  for (uint8_t p = 0; p < n_presets; ++p) {
    const size_t pn = per_preset_count[p];
    if (pn == 0) { s_fits[p] = {}; continue; }

    // Pack contiguous array of just this preset's real shots. Phantoms are
    // added internally by fit_preset.
    auto* this_shots = static_cast<storage::ShotRecord*>(
        std::malloc(sizeof(storage::ShotRecord) * pn));
    if (this_shots == nullptr) { s_fits[p] = {}; continue; }
    size_t k = 0;
    for (size_t i = 0; i < n_total; ++i) {
      const auto& r = shots[i];
      if (r.preset_id != p) continue;
      if (excluded(r)) continue;
      this_shots[k++] = r;
    }
    fit_preset(p, this_shots, k);
    if (s_fits[p].valid) total_used += s_fits[p].n_used;
    std::free(this_shots);
  }

  std::free(shots);

  ESP_LOGI(kTag, "refit: %u records on disk, %u presets fit, %u shots used in fits",
           static_cast<unsigned>(n_total),
           static_cast<unsigned>(n_presets),
           static_cast<unsigned>(total_used));
}

Suggestion suggest_for_preset(uint8_t preset_id) {
  Suggestion none = {std::nanf(""), 0};
  if (s_lock == nullptr || preset_id >= presets::kMaxPresets) return none;

  Guard g;
  if (!g.ok) return none;

  const PresetFit& fit = s_fits[preset_id];
  if (!fit.valid) return none;

  // Need a live climate reading to plug into T/H/P. Without it the model has
  // no x* to evaluate; falling back to means would just regurgitate the
  // training-period average grind, which the user already sees as the
  // preset's grind_anchor.
  const climate::Reading c = climate::latest();
  if (c.timestamp_us == 0) return none;

  // grind_z is unknown — we're solving for it. Plug y=0 (target time hit) into
  // the standardized regression equation:
  //   y_z = β_g·g_z + β_T·T_z + β_H·H_z + β_P·P_z
  //   target y_raw = 0 → target y_z = -mean_y / std_y
  // Rearrange:
  //   g_z = (target_y_z - β_T·T_z - β_H·H_z - β_P·P_z) / β_g
  const float T_z = (c.temp_c       - fit.mean_T) / fit.std_T;
  const float H_z = (c.humidity_pct - fit.mean_H) / fit.std_H;
  const float P_z = (c.pressure_hpa - fit.mean_P) / fit.std_P;
  const float target_y_z = -fit.mean_y / fit.std_y;

  // β_g (grind coefficient) near zero means the model didn't learn a useful
  // grind→time relationship — usually because every shot used the same grind.
  // Dividing by it would produce a wild number with no support. Suppress.
  if (std::fabs(fit.beta[0]) < 1e-3f) return none;

  const float g_z = (target_y_z - fit.beta[1] * T_z
                                - fit.beta[2] * H_z
                                - fit.beta[3] * P_z) / fit.beta[0];

  // De-standardize back to the grinder's dial units.
  float grind = g_z * fit.std_g + fit.mean_g;
  grind = std::clamp(grind, 0.0f, 99.9f);
  grind = std::round(grind * 10.0f) / 10.0f;  // 0.1 step — matches UI stepper

  const float x_star[kNFeat] = {g_z, T_z, H_z, P_z};
  const float v_param        = quad_form(fit.sigma, x_star);
  const uint8_t conf         = confidence_from_variance(v_param);
  if (conf == 0) return none;

  return Suggestion{grind, conf};
}

}  // namespace espressopost::model
