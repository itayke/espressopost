#pragma once

#include "esp_err.h"
#include "lvgl.h"

namespace espressopost::touch {

// Initialize the shared I²C bus, the CST9217 touch controller, and bind it
// to LVGL as a pointer input device.
//
// Must be called after `espressopost::display::init()`.
esp_err_t init(lv_display_t* disp);

}  // namespace espressopost::touch
