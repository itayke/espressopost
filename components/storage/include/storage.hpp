#pragma once

#include <cstdint>

#include "esp_err.h"

namespace espressopost::storage {

// Bit positions for ShotRecord::flags.
//   kFlagTombstone — user deleted this shot; the model must exclude it.
//   kFlagAnomaly   — user marked this shot as an outlier (test pull, channel,
//                    bad puck prep, etc.); excluded from training, kept for
//                    audit.
// Bits 2-7 are reserved. Sync state intentionally does NOT live here —
// append-only LittleFS records stay immutable; cloud sync (when it lands) is
// tracked by a high-water mark in NVS, not per-record.
constexpr uint8_t kFlagTombstone = 1u << 0;
constexpr uint8_t kFlagAnomaly   = 1u << 1;

// Fixed-width on-disk record for one espresso shot.
//
// Layout is packed and explicit so the file is portable and parseable without
// the device — the model (Step 5) and any future host-side tooling read the
// same bytes. Endianness is little-endian (ESP32 native); no host platform we
// care about is big-endian.
//
// `version` lets future schema changes be detected without a separate file
// header. `rtc_epoch_s` is 0 until the PCF85063 driver lands — fall back to
// `timestamp_us` (esp_timer ticks since boot) for ordering in the meantime.
struct __attribute__((packed)) ShotRecord {
  uint8_t  version;          // 2 (current)
  uint8_t  preset_id;        // index into presets::get(); persisted in NVS
  int8_t   time_delta_s;     // user-reported shot time vs target, signed seconds
  uint8_t  quality_stars;    // user-reported taste, 1..5
  int8_t   click_delta;      // model's grind-adjust prediction at shot time, 0 until Step 5
  uint8_t  flags;            // see kFlag* above; 0 = normal, immutable in v1
  uint8_t  _reserved[2];     // future use; zero on write
  int64_t  timestamp_us;     // esp_timer microseconds since boot at submit
  uint32_t rtc_epoch_s;      // wall-clock seconds since 1970 — 0 until RTC step
  float    temp_c;
  float    humidity_pct;
  float    pressure_hpa;
};
static_assert(sizeof(ShotRecord) == 32, "ShotRecord wire size must be stable");

// Mount LittleFS on the `storage` partition (label matches partitions.csv).
// Formats the partition on first boot or after corruption. Safe to call once.
// Returns ESP_ERR_INVALID_STATE on a second call.
esp_err_t init();

// Append one shot record to the log. The record's `version` and `_pad` are
// overwritten internally — callers should leave them zeroed. Other fields are
// passed through verbatim. Flushes to flash before returning, so a power loss
// immediately after this returns will still retain the shot.
esp_err_t append_shot(const ShotRecord& record);

// Total number of records currently in the log. Cheap; reads file size and
// divides by sizeof(ShotRecord). Used by the UI to display "Saved #N".
uint32_t shot_count();

}  // namespace espressopost::storage
