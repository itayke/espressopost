// espressopost — Step 3: shot logging skeleton.
//
// Initializes the AMOLED + touch + LVGL, mounts LittleFS for shot records,
// starts the BME280 climate read loop, and shows the Report screen (time
// delta + 1-5 stars + Submit, append to /littlefs/shots.bin). Climate is
// optional; storage is also non-fatal so a wedged partition doesn't brick
// boot — the UI will just refuse Submit and log the error.

#include "climate.hpp"
#include "display.hpp"
#include "power.hpp"
#include "presets.hpp"
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

  const esp_err_t climate_err = espressopost::climate::init();
  if (climate_err != ESP_OK) {
    ESP_LOGW(kTag, "climate sensor unavailable (%s); records will log 0s",
             esp_err_to_name(climate_err));
  }

  // Power last — installs the idle watchdog after every other subsystem is
  // up. Until init() returns, consume_input() is a no-op, so any stray touch
  // arriving between touch::init and power::init won't try to drive a
  // half-initialized state machine.
  ESP_ERROR_CHECK(espressopost::power::init());

  espressopost::ui::start_report();

  ESP_LOGI(kTag, "report screen up — set delta + stars + submit");
}
