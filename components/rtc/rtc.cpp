#include "rtc.hpp"

#include "board_pins.hpp"
#include "touch.hpp"

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"

#include <cstdio>
#include <cstring>

namespace espressopost::rtc {
namespace {

constexpr const char* kTag = "rtc";

// --- PCF85063 register map (datasheet §8.1) ---
constexpr uint8_t kRegCtrl1   = 0x00;
constexpr uint8_t kRegSeconds = 0x04;  // bit 7 = OS (clock-integrity-lost flag)
// Auto-increment lets us read or write seconds..years in one I²C burst.
constexpr uint8_t kCtrl1Stop  = 1 << 5;
constexpr uint8_t kSecondsOsBit = 1 << 7;

constexpr uint32_t kI2cTimeoutMs = 50;

// PCF85063 year register is 00..99, mapped onto the 21st century.
constexpr int kYearBase = 2000;

i2c_master_dev_handle_t s_dev   = nullptr;
bool                    s_inited = false;
bool                    s_clock_set = false;

uint8_t to_bcd(uint8_t v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); }
uint8_t from_bcd(uint8_t v) { return static_cast<uint8_t>(((v >> 4) * 10) + (v & 0x0F)); }

esp_err_t read_regs(uint8_t reg, uint8_t* dst, size_t n) {
  return i2c_master_transmit_receive(s_dev, &reg, 1, dst, n, kI2cTimeoutMs);
}

esp_err_t write_reg(uint8_t reg, uint8_t val) {
  const uint8_t buf[2] = {reg, val};
  return i2c_master_transmit(s_dev, buf, 2, kI2cTimeoutMs);
}

esp_err_t write_regs(uint8_t reg, const uint8_t* src, size_t n) {
  // i2c_master_transmit needs one contiguous buffer; build [reg, payload...].
  uint8_t buf[1 + 8];  // we only ever burst-write the 7 time bytes
  if (n > sizeof(buf) - 1) return ESP_ERR_INVALID_SIZE;
  buf[0] = reg;
  std::memcpy(buf + 1, src, n);
  return i2c_master_transmit(s_dev, buf, n + 1, kI2cTimeoutMs);
}

// Howard Hinnant's days-from-civil. Returns days from 1970-01-01 (negative
// pre-epoch). Correct for the proleptic Gregorian calendar, no library deps.
int32_t days_from_civil(int y, unsigned m, unsigned d) {
  y -= (m <= 2);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153u * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int>(doe) - 719468;
}

// Inverse — used to format a stored epoch back into human-readable wall time
// for log lines. Same algorithm, run backwards.
void civil_from_days(int32_t z, int* y, unsigned* m, unsigned* d) {
  z += 719468;
  const int era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int yy = static_cast<int>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp < 10 ? mp + 3 : mp - 9;
  *y = yy + (*m <= 2);
}

uint32_t epoch_from_components(int y, unsigned mo, unsigned d, unsigned h,
                               unsigned mi, unsigned s) {
  const int32_t days = days_from_civil(y, mo, d);
  return static_cast<uint32_t>(days * 86400 + static_cast<int32_t>(h * 3600 + mi * 60 + s));
}

// Convert the compiler's __DATE__ ("Mmm dd yyyy") and __TIME__ ("hh:mm:ss")
// literals to a UTC epoch. Treating local build time as UTC is a small lie —
// the resulting clock is wrong by the build machine's TZ offset — but it's
// stable, build-reproducible, and within ~24 h of correct, which is fine as
// a first-boot floor before the user (or NTP companion) sets the real time.
uint32_t build_time_epoch() {
  const char* date = __DATE__;  // "Mmm dd yyyy"
  const char* time = __TIME__;  // "hh:mm:ss"

  static const char kMonths[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  unsigned month = 0;
  for (unsigned i = 0; i < 12; ++i) {
    if (std::strncmp(date, kMonths + i * 3, 3) == 0) {
      month = i + 1;
      break;
    }
  }
  if (month == 0) return 0;

  // __DATE__ pads single-digit days with a leading space: "Jan  5 2026".
  // sscanf handles either form because %d eats whitespace.
  int day = 0, year = 0;
  if (std::sscanf(date + 3, "%d %d", &day, &year) != 2) return 0;

  int h = 0, mi = 0, s = 0;
  if (std::sscanf(time, "%d:%d:%d", &h, &mi, &s) != 3) return 0;

  return epoch_from_components(year, month, static_cast<unsigned>(day),
                               static_cast<unsigned>(h),
                               static_cast<unsigned>(mi),
                               static_cast<unsigned>(s));
}

}  // namespace

esp_err_t init() {
  if (s_inited) return ESP_ERR_INVALID_STATE;

  i2c_master_bus_handle_t bus = touch::i2c_bus();
  if (bus == nullptr) {
    ESP_LOGE(kTag, "touch::i2c_bus() is null — call touch::init() first");
    return ESP_ERR_INVALID_STATE;
  }

  const i2c_device_config_t dev_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address  = board::kRtcI2cAddr,
      .scl_speed_hz    = board::kI2cFreqHz,
  };
  ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &s_dev),
                      kTag, "add_device");

  // Make sure the oscillator is running (STOP=0). On a fresh chip the bit is
  // 0 by default, but a previous firmware crash mid-write could have left it
  // set. Cheap to enforce.
  uint8_t ctrl1 = 0;
  ESP_RETURN_ON_ERROR(read_regs(kRegCtrl1, &ctrl1, 1), kTag, "ctrl1_read");
  if (ctrl1 & kCtrl1Stop) {
    ESP_RETURN_ON_ERROR(write_reg(kRegCtrl1, ctrl1 & ~kCtrl1Stop),
                        kTag, "ctrl1_clear_stop");
  }

  // Probe the OS flag in the seconds register. Set means the oscillator has
  // stopped at some point (cold start / VDD glitch / first-ever power-on) and
  // the chip's time is meaningless. Cleared means a previous run already set
  // the clock and it's been ticking since.
  uint8_t sec_reg = 0;
  ESP_RETURN_ON_ERROR(read_regs(kRegSeconds, &sec_reg, 1), kTag, "sec_read");

  if (sec_reg & kSecondsOsBit) {
    const uint32_t seed = build_time_epoch();
    if (seed == 0) {
      ESP_LOGW(kTag, "OS flag set and build-time epoch parse failed — "
                     "rtc_epoch_s will stay 0 until set_time() is called");
      s_inited = true;
      return ESP_OK;
    }
    ESP_LOGI(kTag, "first boot (OS=1) — seeding from build time %s %s",
             __DATE__, __TIME__);
    s_inited = true;  // set_time requires s_inited.
    const esp_err_t set_err = set_time(seed);
    if (set_err != ESP_OK) {
      ESP_LOGE(kTag, "seed write failed: %s", esp_err_to_name(set_err));
      // Leave s_inited true so callers get well-defined "is_set==false" + 0,
      // not a hang on retry.
      return set_err;
    }
  } else {
    s_clock_set = true;
    s_inited = true;
    const uint32_t now = epoch_s();
    int yy = 0;
    unsigned mo = 0, dd = 0;
    civil_from_days(static_cast<int32_t>(now / 86400), &yy, &mo, &dd);
    const unsigned tod = now % 86400;
    ESP_LOGI(kTag, "PCF85063 already set: %04d-%02u-%02u %02u:%02u:%02u UTC "
                   "(epoch %lu)",
             yy, mo, dd, tod / 3600, (tod / 60) % 60, tod % 60,
             static_cast<unsigned long>(now));
  }

  return ESP_OK;
}

uint32_t epoch_s() {
  if (!s_inited || !s_clock_set) return 0;
  uint8_t r[7];
  if (read_regs(kRegSeconds, r, sizeof(r)) != ESP_OK) return 0;
  // If OS flipped back on between init() and now, the chip's time is stale —
  // surface 0 rather than a bogus value.
  if (r[0] & kSecondsOsBit) {
    s_clock_set = false;
    return 0;
  }
  const unsigned s  = from_bcd(r[0] & 0x7F);
  const unsigned mi = from_bcd(r[1] & 0x7F);
  const unsigned h  = from_bcd(r[2] & 0x3F);
  const unsigned d  = from_bcd(r[3] & 0x3F);
  // r[4] is weekday — derivable from the date; we don't read it back.
  const unsigned mo = from_bcd(r[5] & 0x1F);
  const unsigned yy = from_bcd(r[6]);
  return epoch_from_components(kYearBase + static_cast<int>(yy), mo, d, h, mi, s);
}

bool is_set() { return s_inited && s_clock_set; }

esp_err_t set_time(uint32_t epoch) {
  if (!s_inited) return ESP_ERR_INVALID_STATE;

  const int32_t days = static_cast<int32_t>(epoch / 86400);
  const uint32_t tod = epoch % 86400;
  int yy = 0;
  unsigned mo = 0, dd = 0;
  civil_from_days(days, &yy, &mo, &dd);
  const int year_in_century = yy - kYearBase;
  if (year_in_century < 0 || year_in_century > 99) {
    return ESP_ERR_INVALID_ARG;  // PCF85063 only covers 2000-2099.
  }

  // Zeller's congruence for day-of-week (0 = Sunday). The chip stores it but
  // doesn't validate it; we just need it to be self-consistent.
  int y_ = yy, m_ = static_cast<int>(mo);
  if (m_ < 3) { m_ += 12; y_ -= 1; }
  const int K = y_ % 100, J = y_ / 100;
  const int h = (static_cast<int>(dd) + 13 * (m_ + 1) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;
  const unsigned weekday = static_cast<unsigned>((h + 6) % 7);  // shift so Mon=0 (any consistent choice works)

  // Halt the counter while we rewrite — datasheet §8.2.1 recommends STOP=1
  // for multi-register time writes so the seconds counter doesn't tick
  // through a partially-written value.
  uint8_t ctrl1 = 0;
  ESP_RETURN_ON_ERROR(read_regs(kRegCtrl1, &ctrl1, 1), kTag, "ctrl1_read");
  ESP_RETURN_ON_ERROR(write_reg(kRegCtrl1, ctrl1 | kCtrl1Stop),
                      kTag, "ctrl1_stop");

  const unsigned h_ = tod / 3600;
  const unsigned mi = (tod / 60) % 60;
  const unsigned s  = tod % 60;

  // Writing the seconds register with bit 7 = 0 clears the OS flag — that's
  // the only way to clear it per datasheet §8.4.
  const uint8_t time_regs[7] = {
      static_cast<uint8_t>(to_bcd(static_cast<uint8_t>(s)) & 0x7F),
      to_bcd(static_cast<uint8_t>(mi)),
      to_bcd(static_cast<uint8_t>(h_)),
      to_bcd(static_cast<uint8_t>(dd)),
      static_cast<uint8_t>(weekday & 0x07),
      to_bcd(static_cast<uint8_t>(mo)),
      to_bcd(static_cast<uint8_t>(year_in_century)),
  };
  const esp_err_t wr = write_regs(kRegSeconds, time_regs, sizeof(time_regs));

  // Resume the counter regardless of write outcome so we don't leave the
  // chip halted on a partial failure.
  const esp_err_t rr = write_reg(kRegCtrl1, ctrl1 & ~kCtrl1Stop);

  if (wr != ESP_OK) return wr;
  if (rr != ESP_OK) return rr;

  s_clock_set = true;
  return ESP_OK;
}

}  // namespace espressopost::rtc
