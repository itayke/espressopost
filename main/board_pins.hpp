#pragma once

#include <cstdint>

// Pin map for Waveshare ESP32-S3-Touch-AMOLED-1.75.
// Verbatim from the vendor's reference: examples/Arduino-v3.3.5/libraries/Mylibrary/pin_config.h
// in https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75 (commit at time of import: main).
//
// If a pin behaves unexpectedly, double-check against that file before assuming a code bug.

namespace espressopost::board {

// --- AMOLED display (CO5300) over QSPI on SPI2_HOST ---
inline constexpr int kLcdQspiD0   = 4;
inline constexpr int kLcdQspiD1   = 5;
inline constexpr int kLcdQspiD2   = 6;
inline constexpr int kLcdQspiD3   = 7;
inline constexpr int kLcdQspiSclk = 38;
inline constexpr int kLcdCs       = 12;
inline constexpr int kLcdReset    = 39;
inline constexpr int kLcdTe       = -1;   // Not wired on this board.

inline constexpr int kLcdHRes = 466;
inline constexpr int kLcdVRes = 466;

// --- Shared I²C bus on I2C0: touch + AXP2101 PMIC + PCF85063 RTC + QMI8658 IMU ---
inline constexpr int kI2cSda  = 15;
inline constexpr int kI2cScl  = 14;
inline constexpr int kI2cFreqHz = 400'000;

// --- Touch (CST9217) ---
inline constexpr int kTouchInt   = 11;
inline constexpr int kTouchReset = 40;
// CST9217 7-bit I²C address per Waveshare driver = 0x5A.
inline constexpr uint8_t kTouchI2cAddr = 0x5A;

// --- AXP2101 PMIC ---
// 7-bit I²C address per AXP datasheet = 0x34.
inline constexpr uint8_t kPmicI2cAddr = 0x34;

// --- PCF85063 RTC ---
inline constexpr uint8_t kRtcI2cAddr = 0x51;

// --- QMI8658 IMU ---
// Address depends on SA0 strap; Waveshare wires it as 0x6B.
inline constexpr uint8_t kImuI2cAddr = 0x6B;

// --- External 8-pin header H2 (back of PCB) ---
// Schematic-confirmed pinout: 1=VBUS, 2=GND, 3=VCC3V3, 4=U0RXD, 5=U0TXD,
// 6=GPIO17, 7=GPIO18, 8=GPIO16. Three free GPIOs + UART + power.
//
// BME280 climate sensor (Step 2 of the build order) lives on its own
// I²C bus here — deliberately *not* the internal shared bus (which carries
// PMIC/RTC/IMU/touch). Cleaner electrically, no contention, no address
// conflicts. ESP32-S3's GPIO matrix lets I2C0 or I2C1 route to any pin.
inline constexpr int kExtI2cSda  = 17;   // H2 pin 6
inline constexpr int kExtI2cScl  = 18;   // H2 pin 7
inline constexpr int kExtI2cFreqHz = 400'000;
// GPIO16 (H2 pin 8) intentionally left free for future expansion.
// U0RXD/U0TXD (H2 pins 4/5) intentionally left on UART for serial debugging.

// BME280 7-bit I²C address: 0x76 (SDO=GND) or 0x77 (SDO=VCC). Most
// breakouts ship with SDO pulled low → 0x76. Verify the specific module.
inline constexpr uint8_t kBme280I2cAddr = 0x76;

}  // namespace espressopost::board
