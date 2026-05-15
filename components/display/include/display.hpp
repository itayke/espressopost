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

// Panel brightness in percent (0..100). Sends CO5300 cmd 0x51 with the value
// rescaled to 0..255. AMOLED — 0% is genuinely "all sub-pixels off"; no need
// to also call set_on(false) for power savings, though set_on() additionally
// stops the controller from scanning.
esp_err_t set_brightness(uint8_t pct);

// Panel scan on/off. When off the framebuffer is preserved, so the same UI
// re-appears on the next set_on(true). Use for hard sleep; for soft idle
// just dim via set_brightness().
esp_err_t set_on(bool on);

}  // namespace espressopost::display
