#pragma once

#include <cstdint>

#include "esp_err.h"

namespace espressopost::climate {

// A single climate snapshot. `timestamp_us` is 0 if no successful sample has
// landed yet (sensor missing, init failed, or task hasn't run once). Callers
// should check `timestamp_us != 0` before trusting the float fields.
struct Reading {
  float   temp_c;
  float   humidity_pct;
  float   pressure_hpa;
  int64_t timestamp_us;
};

// Probe the BME280 on the external H2 I²C bus, configure forced-mode sampling,
// and start the background sample task. Safe to call once. Returns
// ESP_ERR_NOT_FOUND if the sensor doesn't respond — the app should treat
// climate as optional and continue without it.
esp_err_t init();

// Cheap snapshot of the most recent successful sample. Protected by a short
// mutex; safe to call from any task including LVGL timers.
Reading latest();

// US-customary display helpers (the UI shows °F and inHg).
inline float c_to_f(float c)        { return c * 1.8f + 32.0f; }
inline float hpa_to_inhg(float hpa) { return hpa * 0.02952998751f; }

}  // namespace espressopost::climate
