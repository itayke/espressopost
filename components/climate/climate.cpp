#include "climate.hpp"

#include "board_pins.hpp"

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <cstring>

namespace espressopost::climate {
namespace {

constexpr const char* kTag = "climate";

// --- BME280 registers (datasheet §5.3) ---
constexpr uint8_t kRegChipId   = 0xD0;  // expect 0x60 for BME280
constexpr uint8_t kRegReset    = 0xE0;  // 0xB6 = soft reset
constexpr uint8_t kRegCtrlHum  = 0xF2;  // osrs_h [2:0]
constexpr uint8_t kRegStatus   = 0xF3;  // measuring bit, im_update bit
constexpr uint8_t kRegCtrlMeas = 0xF4;  // osrs_t [7:5], osrs_p [4:2], mode [1:0]
constexpr uint8_t kRegConfig   = 0xF5;  // t_sb [7:5], filter [4:2], spi3w_en [0]
constexpr uint8_t kRegPressMsb = 0xF7;  // 8 bytes: P[3] T[3] H[2]
constexpr uint8_t kRegCalibA   = 0x88;  // 26 bytes: T1..T3, P1..P9, (rsv), H1
constexpr uint8_t kRegCalibB   = 0xE1;  // 7 bytes: H2..H6

constexpr uint8_t kChipIdBme280 = 0x60;

// Oversampling x1 on all three channels — climate moves slowly, low power wins
// over headline accuracy. Forced mode: each measurement is explicitly
// triggered, then the sensor returns to sleep. At 1 Hz that's ~10 ms active
// + 990 ms sleep.
constexpr uint8_t kCtrlHumX1   = 0x01;             // osrs_h = 001
constexpr uint8_t kCtrlMeasOneShot =
    (0x01 << 5) |  // osrs_t = x1
    (0x01 << 2) |  // osrs_p = x1
    0x01;          // mode = forced

constexpr int kSampleIntervalMs = 1000;
constexpr int kConversionWaitMs = 15;  // datasheet table 13: x1 osr ≈ 9.3 ms

constexpr i2c_port_num_t kI2cPort = I2C_NUM_1;  // I2C_NUM_0 belongs to touch.

// BME280 SDO strap selects between 0x76 (SDO=GND) and 0x77 (SDO=VCC). Many
// breakouts ship with SDO pulled high by default; try both so the user
// doesn't have to read the silkscreen to find out which they got.
constexpr uint8_t kCandidateAddresses[] = {0x76, 0x77};

struct Calibration {
  uint16_t T1;
  int16_t  T2, T3;
  uint16_t P1;
  int16_t  P2, P3, P4, P5, P6, P7, P8, P9;
  uint8_t  H1;
  int16_t  H2;
  uint8_t  H3;
  int16_t  H4, H5;
  int8_t   H6;
};

i2c_master_bus_handle_t s_bus    = nullptr;
i2c_master_dev_handle_t s_dev    = nullptr;
TaskHandle_t            s_task   = nullptr;
SemaphoreHandle_t       s_mutex  = nullptr;
Calibration             s_cal{};
int32_t                 s_t_fine = 0;  // shared between T/P/H compensation
Reading                 s_latest{};    // guarded by s_mutex

esp_err_t read_regs(uint8_t reg, uint8_t* dst, size_t n) {
  return i2c_master_transmit_receive(s_dev, &reg, 1, dst, n, 100);
}

esp_err_t write_reg(uint8_t reg, uint8_t val) {
  const uint8_t buf[2] = {reg, val};
  return i2c_master_transmit(s_dev, buf, 2, 100);
}

esp_err_t load_calibration() {
  uint8_t a[26];
  ESP_RETURN_ON_ERROR(read_regs(kRegCalibA, a, sizeof(a)), kTag, "cal_a");
  s_cal.T1 = static_cast<uint16_t>(a[0] | (a[1] << 8));
  s_cal.T2 = static_cast<int16_t>(a[2] | (a[3] << 8));
  s_cal.T3 = static_cast<int16_t>(a[4] | (a[5] << 8));
  s_cal.P1 = static_cast<uint16_t>(a[6] | (a[7] << 8));
  s_cal.P2 = static_cast<int16_t>(a[8]  | (a[9]  << 8));
  s_cal.P3 = static_cast<int16_t>(a[10] | (a[11] << 8));
  s_cal.P4 = static_cast<int16_t>(a[12] | (a[13] << 8));
  s_cal.P5 = static_cast<int16_t>(a[14] | (a[15] << 8));
  s_cal.P6 = static_cast<int16_t>(a[16] | (a[17] << 8));
  s_cal.P7 = static_cast<int16_t>(a[18] | (a[19] << 8));
  s_cal.P8 = static_cast<int16_t>(a[20] | (a[21] << 8));
  s_cal.P9 = static_cast<int16_t>(a[22] | (a[23] << 8));
  // a[24] is reserved.
  s_cal.H1 = a[25];

  uint8_t b[7];
  ESP_RETURN_ON_ERROR(read_regs(kRegCalibB, b, sizeof(b)), kTag, "cal_b");
  s_cal.H2 = static_cast<int16_t>(b[0] | (b[1] << 8));
  s_cal.H3 = b[2];
  // H4 and H5 share byte b[4]: H4 = b[3]<<4 | b[4]&0x0F, H5 = b[5]<<4 | (b[4]>>4).
  s_cal.H4 = static_cast<int16_t>((static_cast<int16_t>(b[3]) << 4) | (b[4] & 0x0F));
  s_cal.H5 = static_cast<int16_t>((static_cast<int16_t>(b[5]) << 4) | (b[4] >> 4));
  s_cal.H6 = static_cast<int8_t>(b[6]);
  return ESP_OK;
}

// Bosch datasheet §4.2.3 fixed-point compensation. Returns temperature ×100 °C
// (e.g. 5123 = 51.23 °C). Side effect: updates s_t_fine, which P and H need.
int32_t compensate_T(int32_t adc_T) {
  int32_t var1 = ((((adc_T >> 3) - (static_cast<int32_t>(s_cal.T1) << 1))) *
                  static_cast<int32_t>(s_cal.T2)) >> 11;
  int32_t var2 = (((((adc_T >> 4) - static_cast<int32_t>(s_cal.T1)) *
                    ((adc_T >> 4) - static_cast<int32_t>(s_cal.T1))) >> 12) *
                  static_cast<int32_t>(s_cal.T3)) >> 14;
  s_t_fine = var1 + var2;
  return (s_t_fine * 5 + 128) >> 8;
}

// Bosch datasheet §4.2.3 — returns pressure in Q24.8 Pa.
uint32_t compensate_P(int32_t adc_P) {
  int64_t var1 = static_cast<int64_t>(s_t_fine) - 128000;
  int64_t var2 = var1 * var1 * static_cast<int64_t>(s_cal.P6);
  var2 = var2 + ((var1 * static_cast<int64_t>(s_cal.P5)) << 17);
  var2 = var2 + (static_cast<int64_t>(s_cal.P4) << 35);
  var1 = ((var1 * var1 * static_cast<int64_t>(s_cal.P3)) >> 8) +
         ((var1 * static_cast<int64_t>(s_cal.P2)) << 12);
  var1 = (((static_cast<int64_t>(1) << 47) + var1)) *
         static_cast<int64_t>(s_cal.P1) >> 33;
  if (var1 == 0) return 0;
  int64_t p = 1048576 - adc_P;
  p = (((p << 31) - var2) * 3125) / var1;
  var1 = (static_cast<int64_t>(s_cal.P9) * (p >> 13) * (p >> 13)) >> 25;
  var2 = (static_cast<int64_t>(s_cal.P8) * p) >> 19;
  p = ((p + var1 + var2) >> 8) + (static_cast<int64_t>(s_cal.P7) << 4);
  return static_cast<uint32_t>(p);
}

// Bosch datasheet §4.2.3 — returns humidity in Q22.10 %RH (÷1024 for %).
uint32_t compensate_H(int32_t adc_H) {
  int32_t v = s_t_fine - 76800;
  v = (((((adc_H << 14) - (static_cast<int32_t>(s_cal.H4) << 20) -
         (static_cast<int32_t>(s_cal.H5) * v)) + 16384) >> 15) *
       (((((((v * static_cast<int32_t>(s_cal.H6)) >> 10) *
            (((v * static_cast<int32_t>(s_cal.H3)) >> 11) + 32768)) >> 10) +
          2097152) * static_cast<int32_t>(s_cal.H2) + 8192) >> 14));
  v -= (((((v >> 15) * (v >> 15)) >> 7) * static_cast<int32_t>(s_cal.H1)) >> 4);
  if (v < 0) v = 0;
  if (v > 419430400) v = 419430400;
  return static_cast<uint32_t>(v >> 12);
}

esp_err_t sample_once(Reading* out) {
  ESP_RETURN_ON_ERROR(write_reg(kRegCtrlHum, kCtrlHumX1), kTag, "ctrl_hum");
  ESP_RETURN_ON_ERROR(write_reg(kRegCtrlMeas, kCtrlMeasOneShot), kTag, "ctrl_meas");

  vTaskDelay(pdMS_TO_TICKS(kConversionWaitMs));

  // Status bit 3 (measuring) clears when conversion completes; we already
  // waited longer than the worst-case for x1 oversampling, so just read.
  uint8_t raw[8];
  ESP_RETURN_ON_ERROR(read_regs(kRegPressMsb, raw, sizeof(raw)), kTag, "raw");

  const int32_t adc_P = (static_cast<int32_t>(raw[0]) << 12) |
                        (static_cast<int32_t>(raw[1]) << 4)  |
                        (raw[2] >> 4);
  const int32_t adc_T = (static_cast<int32_t>(raw[3]) << 12) |
                        (static_cast<int32_t>(raw[4]) << 4)  |
                        (raw[5] >> 4);
  const int32_t adc_H = (static_cast<int32_t>(raw[6]) << 8) | raw[7];

  // 0x80000 means "skipped/disabled" per datasheet — guards against a sensor
  // that booted with oversampling off (shouldn't happen, but cheap to check).
  if (adc_P == 0x80000 || adc_T == 0x80000 || adc_H == 0x8000) {
    return ESP_ERR_INVALID_RESPONSE;
  }

  const int32_t  t100  = compensate_T(adc_T);      // 0.01 °C
  const uint32_t p_q24 = compensate_P(adc_P);      // Q24.8 Pa
  const uint32_t h_q22 = compensate_H(adc_H);      // Q22.10 %RH

  out->temp_c       = static_cast<float>(t100) / 100.0f;
  out->pressure_hpa = (static_cast<float>(p_q24) / 256.0f) / 100.0f;
  out->humidity_pct = static_cast<float>(h_q22) / 1024.0f;
  out->timestamp_us = esp_timer_get_time();
  return ESP_OK;
}

void sample_task(void* /*arg*/) {
  ESP_LOGI(kTag, "BME280 sample task started (interval %d ms)", kSampleIntervalMs);
  TickType_t last_wake = xTaskGetTickCount();
  for (;;) {
    Reading r{};
    if (sample_once(&r) == ESP_OK) {
      if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_latest = r;
        xSemaphoreGive(s_mutex);
      }
    }
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kSampleIntervalMs));
  }
}

esp_err_t init_bus() {
  const i2c_master_bus_config_t bus_cfg = {
      .i2c_port = kI2cPort,
      .sda_io_num = static_cast<gpio_num_t>(board::kExtI2cSda),
      .scl_io_num = static_cast<gpio_num_t>(board::kExtI2cScl),
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags = {
          .enable_internal_pullup = true,
      },
  };
  return i2c_new_master_bus(&bus_cfg, &s_bus);
}

esp_err_t attach_device(uint8_t addr) {
  const i2c_device_config_t dev_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = addr,
      .scl_speed_hz = board::kExtI2cFreqHz,
  };
  return i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
}

// Returns ESP_OK and writes the matching address to *out if either candidate
// ACKs its 7-bit address; ESP_ERR_NOT_FOUND otherwise. Probing is non-
// intrusive — no device is added to the bus, no register write happens.
esp_err_t find_sensor(uint8_t* out) {
  for (uint8_t a : kCandidateAddresses) {
    const esp_err_t e = i2c_master_probe(s_bus, a, /*timeout_ms=*/50);
    if (e == ESP_OK) {
      *out = a;
      return ESP_OK;
    }
    ESP_LOGD(kTag, "no ACK at 0x%02X (%s)", a, esp_err_to_name(e));
  }
  return ESP_ERR_NOT_FOUND;
}

// Full 7-bit address scan — diagnostic when neither BME280 candidate responds.
// Skips the reserved low (0x00..0x07) and high (0x78..0x7F) ranges. Quiet on
// each individual miss; one summary line at the end.
void scan_bus() {
  ESP_LOGW(kTag, "scanning H2 I²C bus for any responding device...");
  int found = 0;
  for (uint8_t a = 0x08; a <= 0x77; ++a) {
    if (i2c_master_probe(s_bus, a, /*timeout_ms=*/10) == ESP_OK) {
      ESP_LOGW(kTag, "  -> 0x%02X ACKs", a);
      ++found;
    }
  }
  if (found == 0) {
    ESP_LOGW(kTag, "  (bus is silent — check 3V3, GND, and that SDA/SCL "
                   "aren't swapped on H2)");
  }
}

}  // namespace

esp_err_t init() {
  if (s_task != nullptr) return ESP_ERR_INVALID_STATE;

  ESP_RETURN_ON_ERROR(init_bus(), kTag, "bus");

  uint8_t addr = 0;
  if (find_sensor(&addr) != ESP_OK) {
    ESP_LOGW(kTag, "BME280 not found at 0x76 or 0x77 on H2 (SDA=%d SCL=%d)",
             board::kExtI2cSda, board::kExtI2cScl);
    scan_bus();
    return ESP_ERR_NOT_FOUND;
  }

  ESP_RETURN_ON_ERROR(attach_device(addr), kTag, "add_device");

  uint8_t chip_id = 0;
  const esp_err_t chip_err = read_regs(kRegChipId, &chip_id, 1);
  if (chip_err != ESP_OK) {
    ESP_LOGW(kTag, "0x%02X ACKed but chip-id read failed (%s)",
             addr, esp_err_to_name(chip_err));
    return ESP_ERR_NOT_FOUND;
  }
  if (chip_id != kChipIdBme280) {
    ESP_LOGW(kTag, "device at 0x%02X is not a BME280 (chip id 0x%02X)",
             addr, chip_id);
    return ESP_ERR_NOT_FOUND;
  }

  // Soft reset, then wait for the NVM copy to finish (~2 ms per datasheet).
  ESP_RETURN_ON_ERROR(write_reg(kRegReset, 0xB6), kTag, "reset");
  vTaskDelay(pdMS_TO_TICKS(5));

  ESP_RETURN_ON_ERROR(load_calibration(), kTag, "calibration");

  // config: standby unused in forced mode, filter off, SPI3W off.
  ESP_RETURN_ON_ERROR(write_reg(kRegConfig, 0x00), kTag, "config");

  s_mutex = xSemaphoreCreateMutex();
  if (s_mutex == nullptr) return ESP_ERR_NO_MEM;

  const BaseType_t ok = xTaskCreatePinnedToCore(
      sample_task, "climate", /*stack=*/4096, nullptr,
      /*prio=*/3, &s_task, /*core=*/0);
  if (ok != pdPASS) return ESP_ERR_NO_MEM;

  ESP_LOGI(kTag, "BME280 ready at 0x%02X on I2C%d (SDA=%d SCL=%d @ %d Hz)",
           addr, kI2cPort,
           board::kExtI2cSda, board::kExtI2cScl, board::kExtI2cFreqHz);
  return ESP_OK;
}

Reading latest() {
  if (s_mutex == nullptr) return Reading{};
  Reading r{};
  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    r = s_latest;
    xSemaphoreGive(s_mutex);
  }
  return r;
}

}  // namespace espressopost::climate
