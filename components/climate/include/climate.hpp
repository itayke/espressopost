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

// Magnus-Tetens dew point. Inputs in °C and %RH (0..100); output in °C.
// Accurate to ~0.4 °C across normal indoor ranges (T ∈ [0, 40], RH ∈ [10, 95]).
// Returns NaN if RH is non-positive (ln domain).
float dew_point_c(float temp_c, float humidity_pct);

// ---------------------------------------------------------------------------
// Display-format preferences. Each tile on the Report screen's climate strip
// is tappable to flip its unit; the choice persists across reboots (NVS).
// Storage lives in the "climate" namespace under single-letter keys.
//
// Lazily loaded by climate::init(); getters return cached values without
// touching NVS. Setters write through immediately — the tile-tap cadence is
// human-paced, well inside the part's wear budget.
// ---------------------------------------------------------------------------
enum class TempUnit     : uint8_t { Fahrenheit = 0, Celsius = 1 };
enum class PressureUnit : uint8_t { InHg = 0, HPa = 1 };
enum class HumidityUnit : uint8_t { Percent = 0, DewPoint = 1 };

TempUnit     temp_unit();
PressureUnit pressure_unit();
HumidityUnit humidity_unit();
void set_temp_unit(TempUnit);
void set_pressure_unit(PressureUnit);
void set_humidity_unit(HumidityUnit);

}  // namespace espressopost::climate
