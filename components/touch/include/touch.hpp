#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "lvgl.h"

namespace espressopost::touch {

// Initialize the shared I²C bus, the CST9217 touch controller, and bind it
// to LVGL as a pointer input device.
//
// Must be called after `espressopost::display::init()`.
esp_err_t init(lv_display_t* disp);

// Handle to the internal-bus I²C0 master created during init(). Touch is the
// first internal-bus device to come up so it owns the bus init; other
// internal-bus components (RTC, eventually PMIC/IMU) attach devices to this
// handle instead of redundantly creating their own. Returns nullptr before
// init() completes. Hoist into its own `i2c_internal` component when more
// than two consumers exist.
i2c_master_bus_handle_t i2c_bus();

}  // namespace espressopost::touch
