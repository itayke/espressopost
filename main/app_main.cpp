// espressopost — Step 1: hardware bringup.
//
// Initializes the CO5300 AMOLED + CST9217 touch + LVGL 9, then shows a
// single screen with a centered number adjustable by an arc around the
// rim. This is purely a hardware-verification milestone — there's no
// model, no storage, no climate sensor yet. See the kickoff brief's
// "Build order" for what comes next.

#include "display.hpp"
#include "touch.hpp"
#include "ui.hpp"

#include "esp_log.h"

namespace {
constexpr const char* kTag = "app";
}

extern "C" void app_main(void) {
  ESP_LOGI(kTag, "espressopost bringup starting");

  ESP_ERROR_CHECK(espressopost::display::init());
  ESP_ERROR_CHECK(espressopost::touch::init(espressopost::display::lvgl_display()));

  espressopost::ui::start_bringup();

  ESP_LOGI(kTag, "bringup screen up — touch the rim arc to adjust the number");
  // The LVGL task is already running inside the display component; app_main
  // can return without ending the program (FreeRTOS will keep the system
  // alive on the LVGL task + esp_timer service task).
}
