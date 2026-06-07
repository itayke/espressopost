// espressopost — Step 3: shot logging skeleton.
//
// Initializes the AMOLED + touch + LVGL, mounts LittleFS for shot records,
// starts the BME280 climate read loop, and shows the Report screen (brew
// time + 1-5 stars + Submit, append to /littlefs/shots.bin). Climate is
// optional; storage is also non-fatal so a wedged partition doesn't brick
// boot — the UI will just refuse Submit and log the error.

#include "calibration.hpp"
#include "climate.hpp"
#include "cloud.hpp"
#include "display.hpp"
#include "model.hpp"
#include "power.hpp"
#include "presets.hpp"
#include "rtc.hpp"
#include "storage.hpp"
#include "touch.hpp"
#include "ui.hpp"

#include "esp_err.h"
#include "esp_log.h"

namespace {
constexpr const char* kTag = "app";
}

extern "C" void app_main(void) {
  ESP_LOGI(kTag, "espressopost boot");

  ESP_ERROR_CHECK(espressopost::display::init());
  ESP_ERROR_CHECK(espressopost::touch::init(espressopost::display::lvgl_display()));

  const esp_err_t storage_err = espressopost::storage::init();
  if (storage_err != ESP_OK) {
    ESP_LOGE(kTag, "storage init failed (%s) — submit will fail until fixed",
             esp_err_to_name(storage_err));
  }

  // Presets owns nvs_flash_init() right now; hoist it if another NVS user lands.
  const esp_err_t presets_err = espressopost::presets::init();
  if (presets_err != ESP_OK) {
    ESP_LOGE(kTag, "presets init failed (%s) — falling back to defaults in RAM",
             esp_err_to_name(presets_err));
  }

  // Storage's v3/v4 → v5 migration needs preset target_time_s to convert each
  // record's old time delta into an absolute brew time, so it has to run
  // after presets::init() and before model::init() reads the shot log.
  const esp_err_t finalize_err = espressopost::storage::finalize_migrations();
  if (finalize_err != ESP_OK) {
    ESP_LOGE(kTag, "storage finalize failed (%s) — model may misread old records",
             esp_err_to_name(finalize_err));
  }

  const esp_err_t climate_err = espressopost::climate::init();
  if (climate_err != ESP_OK) {
    ESP_LOGW(kTag, "climate sensor unavailable (%s); records will log 0s",
             esp_err_to_name(climate_err));
  }

  // RTC piggybacks on touch's I²C0 bus, so it must come after touch::init.
  // Non-fatal: if it fails (chip missing / wedged), epoch_s() returns 0 and
  // ShotRecord.rtc_epoch_s stays 0 — same as the pre-RTC state.
  const esp_err_t rtc_err = espressopost::rtc::init();
  if (rtc_err != ESP_OK) {
    ESP_LOGW(kTag, "rtc unavailable (%s); shots will log rtc_epoch_s=0",
             esp_err_to_name(rtc_err));
  }

  // Calibration after presets (which owns nvs_flash_init) and before model, so
  // the first refit can bucket shots into epochs. Non-fatal: on failure the list
  // reads empty, which collapses to a single epoch — the pre-feature behavior.
  const esp_err_t calib_err = espressopost::calibration::init();
  if (calib_err != ESP_OK) {
    ESP_LOGW(kTag, "calibration init failed (%s); treating log as one epoch",
             esp_err_to_name(calib_err));
  }

  // Model after storage + presets + climate so its first refit sees the live
  // shot log and can ask climate::latest() during the very next suggest call.
  // Non-fatal: on failure the UI just won't surface a "suggested" row.
  const esp_err_t model_err = espressopost::model::init();
  if (model_err != ESP_OK) {
    ESP_LOGW(kTag, "model unavailable (%s); suggestion row will stay hidden",
             esp_err_to_name(model_err));
  }

  // Cloud after NVS (presets), storage, and RTC are up: it reads the shot log
  // to backfill and relies on stored WiFi creds + endpoint in NVS. Non-fatal —
  // no network just means shots stay queued locally until the next connect.
  const esp_err_t cloud_err = espressopost::cloud::init();
  if (cloud_err != ESP_OK) {
    ESP_LOGW(kTag, "cloud unavailable (%s); shots queue locally until WiFi",
             esp_err_to_name(cloud_err));
  }

  // Power last — installs the idle watchdog after every other subsystem is
  // up. Until init() returns, consume_input() is a no-op, so any stray touch
  // arriving between touch::init and power::init won't try to drive a
  // half-initialized state machine.
  ESP_ERROR_CHECK(espressopost::power::init());

  espressopost::ui::start_report();

  ESP_LOGI(kTag, "report screen up — set brew time + stars + submit");
}
