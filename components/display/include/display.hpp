#pragma once

#include <cstdint>

#include "esp_err.h"
#include "lvgl.h"

namespace espressopost::display {

// Initialize QSPI bus, CO5300 panel, LVGL, and the LVGL task.
// Returns ESP_ERR_INVALID_STATE on a second call.
esp_err_t init();

// LVGL display handle. Valid only after a successful init().
lv_display_t* lvgl_display();

// LVGL is not thread-safe. Any task touching LVGL state (creating widgets,
// reading input, setting values) must hold this lock for the duration of
// that work. The internal LVGL task also holds it across lv_timer_handler().
//
// `timeout_ms == UINT32_MAX` waits forever. Returns true on acquire, false
// on timeout (or if init() hasn't run yet).
bool lock(uint32_t timeout_ms = UINT32_MAX);
void unlock();

}  // namespace espressopost::display
