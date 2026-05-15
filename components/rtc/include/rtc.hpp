#pragma once

#include <cstdint>

#include "esp_err.h"

// PCF85063 real-time clock driver. The chip sits on the shared internal I²C
// bus (touch::i2c_bus()) at address 0x51 and is backed by a CR1220 cell on
// this board, so once seeded it survives power-off and reflashes.
//
// First-boot policy: the PCF85063 powers up with the "OS" (oscillator-stop /
// integrity-lost) bit set, indicating its time is invalid. init() detects
// this and seeds the clock from the firmware's build timestamp (__DATE__ /
// __TIME__ interpreted as UTC). That's approximate — usually minutes to days
// stale by the time the device actually boots — but it gets shots into the
// right ballpark immediately without requiring a UI flow. A precise
// `set_time()` is available for a future setting step (NTP companion app,
// serial command, manual UI) without changing this header.
namespace espressopost::rtc {

// Initialize the I²C device and either seed the clock (first power-on) or
// log the current time (battery-backed boot). Must be called AFTER touch::init
// — touch owns the bus.
esp_err_t init();

// Wall-clock seconds since 1970-01-01 UTC. Returns 0 if init() hasn't run or
// the chip read failed; callers should treat 0 as "no wall clock available"
// and fall back to esp_timer for ordering.
uint32_t epoch_s();

// Has the clock been seeded with something other than the cold-start state?
// True after either a successful first-boot seed or set_time().
bool is_set();

// Override the clock. Useful when the future setting flow lands (NTP, serial,
// UI). Clears the OS flag on success. Returns ESP_ERR_INVALID_STATE if init()
// hasn't run.
esp_err_t set_time(uint32_t epoch_s);

}  // namespace espressopost::rtc
