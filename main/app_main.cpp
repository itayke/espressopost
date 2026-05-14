// espressopost — Steps 1 + 2: hardware bringup + BME280 climate read loop.
//
// Initializes the CO5300 AMOLED + CST9217 touch + LVGL 9, starts a 1 Hz
// BME280 sample task on the H2 header I²C bus, and shows the bringup screen
// (centered number adjustable by a rim arc, climate status strip at the top).
// See the kickoff brief's "Build order" for what comes next.

#include "climate.hpp"
#include "display.hpp"
#include "touch.hpp"
#include "ui.hpp"

#include "esp_err.h"
#include "esp_log.h"

namespace {
constexpr const char* kTag = "app";
}

extern "C" void app_main(void) {
  ESP_LOGI(kTag, "espressopost bringup starting");

  ESP_ERROR_CHECK(espressopost::display::init());
  ESP_ERROR_CHECK(espressopost::touch::init(espressopost::display::lvgl_display()));

  // Climate is optional at the boot level — if the BME280 isn't wired yet, or
  // the H2 header is empty, the rest of the system should still come up. The
  // UI will show dashes in the status strip until a sample lands.
  const esp_err_t climate_err = espressopost::climate::init();
  if (climate_err != ESP_OK) {
    ESP_LOGW(kTag, "climate sensor unavailable (%s); strip will show '--'",
             esp_err_to_name(climate_err));
  }

  espressopost::ui::start_bringup();

  ESP_LOGI(kTag, "bringup screen up — touch the rim arc to adjust the number");
  // The LVGL task is already running inside the display component; app_main
  // can return without ending the program (FreeRTOS will keep the system
  // alive on the LVGL task + esp_timer service task).
}
