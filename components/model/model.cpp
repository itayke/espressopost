#include "model.hpp"
#include "model_math.hpp"

#include <cmath>
#include <cstdlib>

#include "climate.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "presets.hpp"
#include "storage.hpp"

namespace espressopost::model {
namespace {

constexpr const char* kTag = "model";

// Hard cap on shots we load into RAM in one go. 40 B/record × 1024 = 40 KB
// fits comfortably and covers years of typical home use. Going past this would
// only matter for power users running many shots per day — at which point a
// paginated/streaming fit is warranted anyway.
constexpr size_t kMaxShotsLoaded = 1024;

PresetFit         s_fits[presets::kMaxPresets] = {};
SemaphoreHandle_t s_lock = nullptr;

struct Guard {
  bool ok;
  explicit Guard(TickType_t timeout = portMAX_DELAY)
      : ok(xSemaphoreTake(s_lock, timeout) == pdTRUE) {}
  ~Guard() { if (ok) xSemaphoreGive(s_lock); }
};

// Returns true if this shot should be excluded from training. Tombstoned and
// anomaly-flagged records stay on disk for audit but must not influence the
// fit. Shots with no climate reading (timestamp_us==0 means the climate task
// hadn't sampled yet at submit time) are also excluded — three of our four
// features would otherwise read as 0, which both pollutes the fit and skews
// standardization.
bool excluded(const storage::ShotRecord& r) {
  if (r.flags & (storage::kFlagTombstone | storage::kFlagAnomaly)) return true;
  // If both temp and humidity are exactly zero, we never got a real reading
  // for this shot. (Real BME280 output is never exactly 0 for either.)
  if (r.temp_c == 0.0f && r.humidity_pct == 0.0f) return true;
  return false;
}

// Storage-side record → math-side feature row. Lossy on purpose: the math
// only consumes the five features below. Other fields (timestamp, flags,
// quality_stars, suggested_grind) belong to history / future models, not v1.
FitSample to_sample(const storage::ShotRecord& r) {
  return FitSample{
      r.user_grind,
      r.temp_c,
      r.humidity_pct,
      r.pressure_hpa,
      static_cast<float>(r.time_delta_s),
  };
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

    // Pack contiguous array of just this preset's real shots, converted to
    // the math layer's FitSample shape. Phantoms are added inside fit().
    auto* samples = static_cast<FitSample*>(
        std::malloc(sizeof(FitSample) * pn));
    if (samples == nullptr) { s_fits[p] = {}; continue; }
    size_t k = 0;
    for (size_t i = 0; i < n_total; ++i) {
      const auto& r = shots[i];
      if (r.preset_id != p) continue;
      if (excluded(r)) continue;
      samples[k++] = to_sample(r);
    }
    s_fits[p] = fit(samples, k);
    if (s_fits[p].valid) total_used += s_fits[p].n_used;
    std::free(samples);
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

  // Need a live climate reading to plug into T/H/P. Without it the math has
  // no x* to evaluate; falling back to means would just regurgitate the
  // training-period average grind, which the user already sees as the
  // preset's grind_anchor. Check up here (rather than passing NaN climate
  // into the math layer) so the math stays a pure function of its inputs.
  const climate::Reading c = climate::latest();
  if (c.timestamp_us == 0) return none;

  return suggest(s_fits[preset_id],
                 ClimateInput{c.temp_c, c.humidity_pct, c.pressure_hpa});
}

}  // namespace espressopost::model
